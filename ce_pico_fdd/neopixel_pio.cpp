// Neopixel output via PIO. The pin is the WiFi setup button input (SIO) all the time, and becomes the PIO output only
// for the ~30 us of a frame - the button reads H while pressed, the AND gate (IC6) shifts the level to 5V for LED2.
// PIO and not a cycle counted loop like on ce_pico_hdd: exact timing on the RP2350 too, without disabling the IRQs.

#include "pico/stdlib.h"
#include "hardware/pio.h"

#include "defs.h"
#include "utils.h"
#include "neopixel.h"
#include "neopixel.pio.h"

#define FRAME_TIMEOUT_US    100     // 24 bits take 30 us

static PIO pioNeopixel;
static uint smNeopixel;
static uint offsetNeopixel;
static bool ready;

void neopixelOutputInit(void)
{
    ready = pio_claim_free_sm_and_add_program_for_gpio_range(&neopixel_program, &pioNeopixel, &smNeopixel, &offsetNeopixel, PIN_NEOPIXEL, 1, true);
    if(!ready) {
        debug("neopixel - failed to claim a PIO state machine, no status LED\n");
        return;
    }

    neopixel_program_init(pioNeopixel, smNeopixel, offsetNeopixel, PIN_NEOPIXEL);    // doesn't touch the pin function
}

void neopixelOutput(uint32_t grb)
{
    if(!ready) {
        return;
    }

    uint32_t stallMask = 1u << (PIO_FDEBUG_TXSTALL_LSB + smNeopixel);

    pio_gpio_init(pioNeopixel, PIN_NEOPIXEL);           // the pin to PIO, driven L by the stalled SM
    pio_sm_put(pioNeopixel, smNeopixel, grb << 8);      // 24 bits, MSB first

    uint32_t start = time_us_32();
    while(!pio_sm_is_tx_fifo_empty(pioNeopixel, smNeopixel) && (time_us_32() - start) < FRAME_TIMEOUT_US) {
        tight_loop_contents();                          // the SM took the bits - it's not stalled anymore
    }

    pioNeopixel->fdebug = stallMask;                    // clear the stall flag - set again after the last bit
    while(!(pioNeopixel->fdebug & stallMask) && (time_us_32() - start) < FRAME_TIMEOUT_US) {
        tight_loop_contents();
    }

    gpio_set_function(PIN_NEOPIXEL, GPIO_FUNC_SIO);     // back to the button input (SIO direction stays in)
}
