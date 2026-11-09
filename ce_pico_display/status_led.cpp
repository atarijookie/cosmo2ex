// Neopixel output via PIO - the same program as ce_pico_fdd (neopixel.pio, keep them in sync). Unlike on the FDD,
// the pin is only the LED's, so it stays PIO all the time.

#include "pico/stdlib.h"
#include "hardware/pio.h"

#include "defs.h"
#include "status_led.h"
#include "neopixel.pio.h"

static PIO pioLed;
static uint smLed;
static uint offsetLed;
static bool ready;

void ledInit(void)
{
    ready = pio_claim_free_sm_and_add_program_for_gpio_range(&neopixel_program, &pioLed, &smLed, &offsetLed,
                                                             PIN_NEOPIXEL, 1, true);
    if(!ready) {
        return;
    }

    neopixel_program_init(pioLed, smLed, offsetLed, PIN_NEOPIXEL);
    pio_gpio_init(pioLed, PIN_NEOPIXEL);
    busy_wait_us_32(300);               // L for 280+ us - reset before the first frame
}

void ledSet(uint8_t r, uint8_t g, uint8_t b)
{
    if(!ready) {
        return;
    }

#if NEOPIXEL_RGB
    uint32_t color = (r << 16) | (g << 8) | b;
#else
    uint32_t color = (g << 16) | (r << 8) | b;
#endif

    pio_sm_put_blocking(pioLed, smLed, color << 8);     // 24 bits, MSB first
    busy_wait_us_32(30 + 300);          // the frame, then L for 280+ us latches it - another frame right after this
                                        // one would be taken as the next LED's bits, not as a new color
}
