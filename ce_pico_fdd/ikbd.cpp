// IKBD (keyboard + mouse) between the keyboard, the ST and the host - like on ce_pico_hdd:
// - KEYB_TX_ORIG (GP9, from the keyboard) and KEYB_TX (GP8, to the ST) are the pins of uart1
// - KEYB_RX (GP18, the ST's commands to the keyboard, only listened to) has no uart, it's a PIO receiver
// The board is in the keyboard -> ST path, so while not forwarding to the host (IKBD disabled, or no connection to the
// host), the uart IRQ passes the bytes from the keyboard to the ST itself - the core 0 main loop can be busy for
// seconds (joining the WiFi, connecting), the keyboard must not stop for that. With IKBD enabled and connected, the
// keyboard data and the ST commands go to the host (tagged), and what the host sends goes to the ST.

#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "settings.h"
#include "ikbd.h"
#include "uart_rx.pio.h"

#define IKBD_QUEUE_SIZE     512     // 0.65 s of data at 7812 baud - the main loop is sometimes busy for a while
#define IKBD_BATCH          64      // bytes per pass in each direction
#define IKBD_RUN_MS         10      // processing not more often than this
#define IKBD_CONNECT_MS     3000    // between connect attempts - connect() blocks the main loop for up to 3 s

extern IPAddress hostIpAddr;
extern uint16_t hostPortIkbd;
extern bool connectedToHost;        // the fdd connection - the host's ikbd port is known then

static TConnection connectionIkbd;

static queue_t fifoFromKeyb;        // KEYB_TX_ORIG -> host
static queue_t fifoFromSt;          // KEYB_RX -> host
static queue_t fifoToSt;            // host -> KEYB_TX

static PIO pioKeybRx;
static uint smKeybRx;

static volatile bool passThrough = true;        // keyboard -> ST right in the IRQ
static volatile uint32_t droppedCount = 0;      // incremented in the IRQs (no debug() there), reported once a second

// uart1 RX: from the keyboard
static void uartIrq(void)
{
    while(uart_is_readable(uart1)) {
        uint8_t c = uart_getc(uart1);

        if(passThrough) {
            if(uart_is_writable(uart1)) {       // same baud rate in and out - the fifo doesn't fill up
                uart_putc_raw(uart1, c);
            } else {
                droppedCount++;
            }
        } else if(!queue_try_add(&fifoFromKeyb, &c)) {
            droppedCount++;
        }
    }
}

// PIO RX: from the ST - only needed while forwarding to the host
static void pioIrq(void)
{
    while(!pio_sm_is_rx_fifo_empty(pioKeybRx, smKeybRx)) {
        char c = uart_rx_program_getc(pioKeybRx, smKeybRx);

        if(!passThrough && !queue_try_add(&fifoFromSt, &c)) {
            droppedCount++;
        }
    }
}

void ikbdInit(void)
{
    queue_init(&fifoFromKeyb, 1, IKBD_QUEUE_SIZE);
    queue_init(&fifoFromSt, 1, IKBD_QUEUE_SIZE);
    queue_init(&fifoToSt, 1, IKBD_QUEUE_SIZE);

    // KEYB_TX + KEYB_TX_ORIG - uart1
    uart_init(uart1, IKBD_BAUD_RATE);
    gpio_set_function(PIN_KEYB_TX, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX));
    gpio_set_function(PIN_KEYB_TX_ORIG, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX_ORIG));

    irq_set_exclusive_handler(UART1_IRQ, uartIrq);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irqs_enabled(uart1, true, false);     // RX: 4 bytes in the fifo, or 1+ and quiet for 32 bit times (4 ms)

    // KEYB_RX - PIO
    uint offset;
    if(!pio_claim_free_sm_and_add_program_for_gpio_range(&uart_rx_program, &pioKeybRx, &smKeybRx, &offset, PIN_KEYB_RX, 1, true)) {
        debug("ikbd - failed to claim a PIO state machine, ST commands won't reach the host\n");
        return;
    }
    uart_rx_program_init(pioKeybRx, smKeybRx, offset, PIN_KEYB_RX, IKBD_BAUD_RATE);

    // IRQ 1 of the PIO - SerialPIO (front panel) takes IRQ 0 of its PIO as exclusive, this might be the same PIO
    uint pioIrqNum = pio_get_irq_num(pioKeybRx, 1);
    irq_add_shared_handler(pioIrqNum, pioIrq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(pioIrqNum, true);
    pio_set_irqn_source_enabled(pioKeybRx, 1, pio_get_rx_fifo_not_empty_interrupt_source(smKeybRx), true);
}

static void dropQueue(queue_t* q)
{
    uint8_t c;
    while(queue_try_remove(q, &c)) {
    }
}

// Switching to / from the pass through in the IRQ. Back to pass through: what the keyboard sent and the host didn't get
// goes to the ST first (as much as the uart fifo takes), what the host sent and the ST didn't get is dropped.
static void setPassThrough(bool on)
{
    if(on == passThrough) {
        return;
    }

    irq_set_enabled(UART1_IRQ, false);

    if(on) {
        uint8_t c;
        while(queue_try_remove(&fifoFromKeyb, &c)) {
            if(uart_is_writable(uart1)) {
                uart_putc_raw(uart1, c);
            }
        }
        dropQueue(&fifoToSt);
        dropQueue(&fifoFromSt);
    }

    passThrough = on;
    irq_set_enabled(UART1_IRQ, true);

    debug("ikbd - %s\n", on ? "keyboard straight to the ST" : "forwarding to the host");
}

static void ikbdConnectDisconnect(uint32_t now)
{
    static uint32_t lastAttempt = 0xffff0000;   // -65k

    bool wanted = Settings.ikbdEnabled && connectedToHost && hostPortIkbd != 0;

    if(!wanted) {
        if(isConnected(&connectionIkbd)) {
            debug("ikbd - disconnect\n");
            stop(&connectionIkbd);
        }
        return;
    }

    if(isConnected(&connectionIkbd) || (now - lastAttempt) < IKBD_CONNECT_MS) {
        return;
    }
    lastAttempt = now;

    debug("ikbd - connecting to port %u\n", hostPortIkbd);
    connect(&connectionIkbd, hostIpAddr, hostPortIkbd);
    if(isConnected(&connectionIkbd)) {
        connectionIkbd.setSync(false);      // not waiting for the ack of every few bytes - forwarded about every 10 ms
    }
}

// one pass of forwarding: keyboard + ST -> host (tagged, in one write), host -> ST
static void forward(void)
{
    uint8_t out[2 * IKBD_BATCH];
    size_t len = 0;
    uint8_t c;

    while(len < sizeof(out) && queue_try_remove(&fifoFromKeyb, &c)) {
        out[len++] = UARTMARK_KEYBDATA;
        out[len++] = c;
    }

    while(len < sizeof(out) && queue_try_remove(&fifoFromSt, &c)) {
        out[len++] = UARTMARK_STCMD;
        out[len++] = c;
    }

    if(len) {
        conWrite(&connectionIkbd, out, len);
    }

    uint8_t in[IKBD_BATCH];
    size_t room = IKBD_QUEUE_SIZE - queue_get_level(&fifoToSt);
    size_t got = conRead(&connectionIkbd, in, MIN(sizeof(in), room));
    for(size_t i = 0; i < got; i++) {
        queue_try_add(&fifoToSt, &in[i]);
    }

    while(uart_is_writable(uart1) && queue_try_remove(&fifoToSt, &c)) {
        uart_putc_raw(uart1, c);
    }
}

// the host connection is there, but IKBD is disabled - drop what the host sends
static void dropFromHost(void)
{
    uint8_t in[IKBD_BATCH];
    while(conRead(&connectionIkbd, in, sizeof(in)) > 0) {
    }
}

void ikbdProcessing(void)
{
    static uint32_t lastRun = 0;
    static uint32_t lastStatus = 0;
    static uint32_t droppedReported = 0;

    uint32_t now = millis();
    if((now - lastRun) < IKBD_RUN_MS) {
        return;
    }
    lastRun = now;

    if((now - lastStatus) >= 1000) {        // once per second
        lastStatus = now;

        uint32_t dropped = droppedCount;
        if(dropped != droppedReported) {
            debug("ikbd - %u bytes dropped\n", dropped - droppedReported);
            droppedReported = dropped;
        }

        if(isConnected(&connectionIkbd)) {  // keep alive
            uint8_t data[2] = {UARTMARK_ALIVE, UARTMARK_ALIVE};
            conWrite(&connectionIkbd, data, 2);
        }
    }

    ikbdConnectDisconnect(now);

    bool forwarding = Settings.ikbdEnabled && isConnected(&connectionIkbd);
    setPassThrough(!forwarding);

    if(forwarding) {
        forward();
    } else if(isConnected(&connectionIkbd)) {
        dropFromHost();
    }
}
