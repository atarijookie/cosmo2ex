#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"

#include "defs.h"
#include "neopixel.h"
#include "bridge.h"
#include "utils.h"

#define LED_STATE_INIT      0
#define LED_STATE_ERROR     1
#define LED_STATE_RUNNING   2

static volatile uint8_t ledState = LED_STATE_INIT;
static volatile uint8_t ledInitStep = 0;
static volatile uint8_t ledErrorCode = 0;

// colors at full brightness of the status - the WS2812C-2020 is bright, so these are far from 255
#define ORANGE_R            96
#define ORANGE_G            28
#define GREEN_G             48
#define RED_R               64

// error blinking: code x (on + off), then pause
#define ERR_BLINK_ON_MS     250
#define ERR_BLINK_OFF_MS    250
#define ERR_PAUSE_MS        1500

#define REFRESH_MS          1000    // during init / error send the color again even if unchanged - fixes the color after junk bits
                                    // from shared pin activity; once running (green) it's sent just once, so the bus isn't disturbed

// WS2812C-2020 bit timing in ns (T0H 220-380, T1H / T0L / T1L 580-1000)
#define T0H_NS              250
#define T0L_NS              800
#define T1H_NS              700
#define T1L_NS              600

static uint32_t cyclesT0H, cyclesT0L, cyclesT1H, cyclesT1L;

void ledSetInitStep(uint8_t step)
{
    ledInitStep = step;
    ledErrorCode = 0;
    ledState = LED_STATE_INIT;
}

void ledSetError(uint8_t code)
{
    ledErrorCode = code;
    ledState = LED_STATE_ERROR;
}

void ledSetRunning(void)
{
    ledErrorCode = 0;
    ledState = LED_STATE_RUNNING;
}

void haltWithError(uint8_t code)
{
    ledSetError(code);

    if(get_core_num() == 1) {       // core 1 is the one sending the status to neopixel, keep doing it
        while(1) {
            neopixelProcess();
        }
    }

    while(1) {                      // core 0 - core 1 shows the status, just keep the debug output going
#ifdef LOG_LED
        debugFromQueue();
#endif
        sleep_ms(100);
    }
}

static uint32_t toGrb(uint8_t r, uint8_t g, uint8_t b)
{
    return (((uint32_t) g) << 16) | (((uint32_t) r) << 8) | b;
}

static uint32_t currentColor(uint32_t now)
{
    switch(ledState) {
        case LED_STATE_RUNNING:
            return toGrb(0, GREEN_G, 0);

        case LED_STATE_ERROR: {
            uint32_t code = ledErrorCode;
            uint32_t blinksTime = code * (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS);
            uint32_t t = now % (blinksTime + ERR_PAUSE_MS);

            bool on = (t < blinksTime) && ((t % (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS)) < ERR_BLINK_ON_MS);
            return on ? toGrb(RED_R, 0, 0) : 0;
        }

        default: {                  // init - orange, brighter with each step
            uint32_t step = MIN(MAX(ledInitStep, 1), LED_STEP_COUNT);
            return toGrb((ORANGE_R * step) / LED_STEP_COUNT, (ORANGE_G * step) / LED_STEP_COUNT, 0);
        }
    }
}

void neopixelInit(void)
{
    uint32_t mhz = clock_get_hz(clk_sys) / 1000000;
    cyclesT0H = (T0H_NS * mhz) / 1000;
    cyclesT0L = (T0L_NS * mhz) / 1000;
    cyclesT1H = (T1H_NS * mhz) / 1000;
    cyclesT1L = (T1L_NS * mhz) / 1000;

    if(IS_ACSI) {                   // ACSI - the pin is just for the neopixel, idle L
        gpio_init(PIN_NEOPIXEL);
        gpio_put(PIN_NEOPIXEL, 0);
        gpio_set_dir(PIN_NEOPIXEL, GPIO_OUT);
    }                               // SCSI - the pin is set up by the bridge before each frame
}

void neopixelProcess(void)
{
    static uint32_t lastColor = 0xffffffff;
    static uint32_t lastSentTime = 0;

    uint32_t now = millis();
    uint32_t color = currentColor(now);

    bool refreshDue = (ledState != LED_STATE_RUNNING) && (now - lastSentTime) >= REFRESH_MS;

    if(color == lastColor && !refreshDue) {     // nothing changed, refresh not needed (yet)
        return;
    }

    neopixelOutput(color);
    lastColor = color;
    lastSentTime = now;
}

// 3 cycles per loop, like busy_wait_at_least_cycles(), but always inlined - so it runs from RAM with the caller
static inline __attribute__((always_inline)) void delayCycles(uint32_t cycles)
{
    __asm volatile (".syntax unified\n"
                    "1: subs %0, #3\n"
                    "   bcs 1b\n" : "+l" (cycles) : : "cc", "memory");
}

// From RAM with interrupts disabled - a flash cache miss or an IRQ during a bit would change it (~31 us in total).
void __not_in_flash_func(neopixelSendFrame)(uint32_t grb)
{
    uint32_t mask = 1u << PIN_NEOPIXEL;
    uint32_t irqState = save_and_disable_interrupts();

    for(int i = 23; i >= 0; i--) {      // MSB first, G R B
        bool one = (grb >> i) & 1;

        sio_hw->gpio_set = mask;
        delayCycles(one ? cyclesT1H : cyclesT0H);
        sio_hw->gpio_clr = mask;
        delayCycles(one ? cyclesT1L : cyclesT0L);
    }

    restore_interrupts(irqState);
}
