#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "defs.h"
#include "utils.h"
#include "beeper.h"

static volatile uint16_t requestMs;     // from core 0, 0 = none
static volatile bool stepClick = true;  // from core 0 - the setting beep on step
static volatile bool clickOn;           // a step click is on now - its alarm ends it
static alarm_pool_t* clickAlarmPool;    // on core 1, so the alarm IRQ comes there
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
    stepClick = true;
    clickOn = false;

    gpio_init(PIN_BEEPER);
    gpio_set_dir(PIN_BEEPER, GPIO_OUT);
    gpio_put(PIN_BEEPER, 0);
}

void beep(uint16_t ms)
{
    requestMs = ms;                     // one not picked up yet is replaced
}

void beeperSetStepClick(bool enabled)
{
    stepClick = enabled;
}

void beeperCore1Init(void)
{
    clickAlarmPool = alarm_pool_create_with_unused_hardware_alarm(1);   // one click at a time
}

// alarm IRQ on core 1 - the end of a step click
static int64_t clickEnd(alarm_id_t id, void* userData)
{
    clickOn = false;

    if(!on) {                   // a longer beep started meanwhile? it goes on
        gpio_put(PIN_BEEPER, 0);
    }
    return 0;                   // don't repeat
}

void beeperStepClick(void)
{
    // switched off, no alarm to end it, a longer beep on (the click wouldn't be heard), or the previous click still on
    // (steps closer than BEEP_STEP_US - it isn't made longer, each step is still heard as its own click)
    if(!stepClick || clickAlarmPool == NULL || on || clickOn) {
        return;
    }

    clickOn = true;
    gpio_put(PIN_BEEPER, 1);

    if(alarm_pool_add_alarm_in_us(clickAlarmPool, BEEP_STEP_US, clickEnd, NULL, true) < 0) {   // no alarm? no click
        clickOn = false;
        gpio_put(PIN_BEEPER, 0);
    }
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
