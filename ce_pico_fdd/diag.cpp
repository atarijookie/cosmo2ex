#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/watchdog.h"

#include "defs.h"
#include "utils.h"
#include "diag.h"

#define BOOT_COUNT_MAGIC    0xb0070000u     // upper half in watchdog scratch 0 - a valid count, lower half the count
#define BOOT_COUNT_MASK     0x0000ffffu

void diagPrintBootInfo(void)
{
    uint32_t scratch = watchdog_hw->scratch[0];
    uint32_t count = ((scratch & ~BOOT_COUNT_MASK) == BOOT_COUNT_MAGIC) ? (scratch & BOOT_COUNT_MASK) + 1 : 1;
    watchdog_hw->scratch[0] = BOOT_COUNT_MAGIC | (count & BOOT_COUNT_MASK);

    debug("boot #%u since power on, powman chip_reset: %08x, watchdog reason: %08x\n", (unsigned) count,
          (unsigned) powman_hw->chip_reset, (unsigned) watchdog_hw->reason);
}

// The fault output can't use debug() - vsnprintf() with whatever state the crash left behind. Straight to the UART.
static void faultPutStr(const char* s)
{
    while(*s) {
        if(*s == '\n') {
            uart_putc_raw(uart0, '\r');
        }
        uart_putc_raw(uart0, *s++);
    }
}

static void faultPutHex(const char* name, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[9];

    for(int i = 7; i >= 0; i--) {
        buf[i] = digits[value & 0xf];
        value >>= 4;
    }
    buf[8] = 0;

    faultPutStr(name);
    faultPutStr(buf);
}

extern "C" void diagHardFault(uint32_t* frame, uint32_t excReturn)
{
    faultPutStr("\n\n*** HARD FAULT on core ");
    faultPutStr(get_core_num() ? "1" : "0");
    faultPutHex("\npc: ", frame[6]);
    faultPutHex("  lr: ", frame[5]);
    faultPutHex("  xpsr: ", frame[7]);
    faultPutHex("\nr0: ", frame[0]);
    faultPutHex("  r1: ", frame[1]);
    faultPutHex("  r2: ", frame[2]);
    faultPutHex("  r3: ", frame[3]);
    faultPutHex("  r12: ", frame[4]);
    faultPutHex("\nsp: ", (uint32_t) frame);
    faultPutHex("  exc_return: ", excReturn);
    faultPutHex("\ncfsr: ", *(volatile uint32_t*) 0xE000ED28);
    faultPutHex("  hfsr: ", *(volatile uint32_t*) 0xE000ED2C);
    faultPutHex("  mmfar: ", *(volatile uint32_t*) 0xE000ED34);
    faultPutHex("  bfar: ", *(volatile uint32_t*) 0xE000ED38);
    faultPutStr("\n*** this core stopped\n");

    while(true) {
        tight_loop_contents();
    }
}

// replaces the weak isr_hardfault of the pico-sdk crt0 - passes the stacked registers (MSP or PSP, by EXC_RETURN bit 2)
extern "C" __attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4         \n"
        "ite eq             \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "mov r1, lr         \n"
        "b diagHardFault    \n"
    );
}
