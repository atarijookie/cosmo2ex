#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "defs.h"
#include "utils.h"
#include "beeper.h"

static volatile uint16_t requestMs;     // from core 0, 0 = none
static bool on;
static uint32_t offTime;

// on core 1 only - in the STEP ISR, or in the loop with the IRQs off
static void start(uint32_t now, uint16_t ms)
{
    uint32_t until = now + ms;

    if(!on || (int32_t) (until - offTime) > 0) {
        offTime = until;
    }

    on = true;
    gpio_put(PIN_BEEPER, 1);
}

void beeperInit(void)
{
    requestMs = 0;
    on = false;

    gpio_init(PIN_BEEPER);
    gpio_set_dir(PIN_BEEPER, GPIO_OUT);
    gpio_put(PIN_BEEPER, 0);
}

void beep(uint16_t ms)
{
    requestMs = ms;                     // one not picked up yet is replaced
}

void beeperStepClick(void)
{
    start(millis(), BEEP_STEP_MS);
}

void beeperLoop(void)
{
    uint32_t irqState = save_and_disable_interrupts();     // the STEP ISR starts beeps too
    uint32_t now = millis();

    uint16_t ms = requestMs;
    if(ms) {
        requestMs = 0;
        start(now, ms);
    }

    if(on && (int32_t) (now - offTime) >= 0) {
        on = false;
        gpio_put(PIN_BEEPER, 0);
    }

    restore_interrupts(irqState);
}
