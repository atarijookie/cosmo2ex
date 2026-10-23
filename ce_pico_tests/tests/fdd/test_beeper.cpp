// beeper.cpp - buzzer with its own generator on PIN_BEEPER, on core 1: a click on every head step (from the STEP ISR),
// the beeps asked for by core 0 start in the core 1 loop, and every beep ends there after its length - a shorter one
// never cuts a longer one short.

#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "beeper.h"

void floppyStepISR(uint gpio, uint32_t event_mask);    // core1_main.cpp

namespace {

struct BeeperFixture {
    BeeperFixture() {
        fakeReset();
        fakeMillisStep = 0;
        fakeNowMs += 100;                   // away from the last step, not a glitch
        fakeGpioLevel[PIN_MOT_EN] = false;  // motor on
        beeperInit();
    }
};

void step(void)
{
    floppyStepISR(PIN_STEP, GPIO_IRQ_EDGE_FALL);
}

bool beeping(void)
{
    return fakeGpioLevel[PIN_BEEPER];
}

// loop for ms, return how long the beeper was on
uint32_t runLoop(uint32_t ms)
{
    uint32_t onMs = 0;
    for (uint32_t i = 0; i < ms; i++) {
        beeperLoop();
        onMs += beeping() ? 1 : 0;
        fakeNowMs++;
    }
    return onMs;
}

}   // namespace

TEST_SUITE("fdd beeper") {

TEST_CASE_FIXTURE(BeeperFixture, "off after init") {
    CHECK_FALSE(beeping());
    CHECK(runLoop(10) == 0);
}

TEST_CASE_FIXTURE(BeeperFixture, "a step clicks right in the ISR, the loop ends the click") {
    step();
    CHECK(beeping());
    CHECK(runLoop(10) == BEEP_STEP_MS);
    CHECK_FALSE(beeping());
}

TEST_CASE_FIXTURE(BeeperFixture, "no click on a step with the motor off - the head doesn't move") {
    fakeGpioLevel[PIN_MOT_EN] = true;
    step();
    CHECK_FALSE(beeping());
}

TEST_CASE_FIXTURE(BeeperFixture, "a beep from core 0 starts in the core 1 loop and lasts its length") {
    beep(BEEP_IMAGE_LOADED_MS);
    CHECK_FALSE(beeping());                 // the pin is core 1's
    CHECK(runLoop(200) == BEEP_IMAGE_LOADED_MS);
    CHECK_FALSE(beeping());

    CHECK(runLoop(200) == 0);               // picked up only once
}

TEST_CASE_FIXTURE(BeeperFixture, "a step click doesn't cut a longer beep short") {
    beep(BEEP_HOST_LOST_MS);
    CHECK(runLoop(10) == 10);
    step();
    CHECK(runLoop(1000) == BEEP_HOST_LOST_MS - 10);
}

TEST_CASE_FIXTURE(BeeperFixture, "a beep during a click is as long as asked") {
    step();
    beep(BEEP_HOST_CONNECTED_MS);
    CHECK(runLoop(1000) == BEEP_HOST_CONNECTED_MS);
}

TEST_CASE_FIXTURE(BeeperFixture, "interrupts are on again after the loop") {
    beep(BEEP_IMAGE_LOADED_MS);
    beeperLoop();
    CHECK_FALSE(fakeIrqsDisabled);          // restored
}

}
