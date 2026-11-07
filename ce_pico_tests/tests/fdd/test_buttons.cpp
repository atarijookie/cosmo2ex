// buttons.cpp - the button on the FDD board: held long enough it beeps and the LED goes light blue, and only the release
// starts / leaves the captive portal. The long press of enter on the front panel does it right away.

#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_neopixel.h"

// onPanelKey() is faked for the panel tests (fake_panel_uart.cpp) - the real one is called realOnPanelKey here
#define onPanelKey realOnPanelKey
#include "buttons.cpp"
#undef onPanelKey

#include "defs.h"
#include "beeper.h"
#include "neopixel.h"

// captive_portal.cpp needs the WiFi - faked here
static int toggleCount;
static bool canToggle;

void toggleCaptivePortal(void)
{
    toggleCount++;
}

bool captivePortalCanToggle(void)
{
    return canToggle;
}

namespace {

const uint32_t LIGHT_BLUE = (32u << 16) | 64u;     // grb

struct ButtonFixture {
    int beeps = 0;
    bool beeperWasOn = false;

    ButtonFixture() {
        fakeReset();
        fakeMillisStep = 0;
        toggleCount = 0;
        canToggle = true;
        beeperInit();
        ledSetInitStep(LED_STEP_SETUP);
        neopixelInit();
        run(5);
    }

    ~ButtonFixture() {                      // handleAllButtons() keeps its state - leave it released
        fakeGpioSet(PIN_CAPTIVE_BTN, false);
        run(BTN_DEBOUNCE_MS + 2);
    }

    // main loop passes, 1 ms apart
    void run(uint32_t ms) {
        for (uint32_t i = 0; i < ms; i++) {
            handleAllButtons();
            neopixelProcess();
            beeperLoop();

            bool on = fakeGpioLevel[PIN_BEEPER];
            if (on && !beeperWasOn) {
                beeps++;
            }
            beeperWasOn = on;

            fakeNowMs++;
        }
    }

    void hold(uint32_t ms) {
        fakeGpioSet(PIN_CAPTIVE_BTN, true);
        run(ms);
    }

    void release(void) {
        fakeGpioSet(PIN_CAPTIVE_BTN, false);
        run(BTN_DEBOUNCE_MS + 2);
    }

    uint32_t lastColor(void) {
        REQUIRE(!fakeNeopixelFrames.empty());
        return fakeNeopixelFrames.back().grb;
    }
};

}   // namespace

TEST_SUITE("fdd buttons") {

TEST_CASE_FIXTURE(ButtonFixture, "short press does nothing") {
    uint32_t color = lastColor();

    hold(BTN_LONG_PRESS_MS - 50);
    CHECK(beeps == 0);
    CHECK(lastColor() == color);

    release();
    CHECK(toggleCount == 0);
    CHECK(beeps == 0);
}

TEST_CASE_FIXTURE(ButtonFixture, "long press - beep and light blue once, right at the time, the portal not yet") {
    hold(BTN_LONG_PRESS_MS + BTN_DEBOUNCE_MS + 5);
    CHECK(beeps == 1);
    CHECK(lastColor() == LIGHT_BLUE);
    CHECK(toggleCount == 0);

    run(5000);                              // held for long - the same
    CHECK(beeps == 1);
    CHECK(toggleCount == 0);
}

TEST_CASE_FIXTURE(ButtonFixture, "the release starts the portal, once, and the LED goes back") {
    uint32_t color = lastColor();

    hold(BTN_LONG_PRESS_MS + 100);
    release();
    CHECK(toggleCount == 1);
    CHECK(lastColor() == color);

    run(2000);
    CHECK(toggleCount == 1);
}

TEST_CASE_FIXTURE(ButtonFixture, "the next long press works the same") {
    hold(BTN_LONG_PRESS_MS + 100);
    release();
    hold(BTN_LONG_PRESS_MS + 100);
    release();
    CHECK(toggleCount == 2);
    CHECK(beeps == 2);
}

TEST_CASE_FIXTURE(ButtonFixture, "no feedback and no toggle when the portal can't be toggled") {
    uint32_t color = lastColor();
    canToggle = false;

    hold(BTN_LONG_PRESS_MS + 100);
    CHECK(beeps == 0);
    CHECK(lastColor() == color);

    release();
    CHECK(toggleCount == 0);
}

TEST_CASE_FIXTURE(ButtonFixture, "a bounce shorter than the debounce time isn't a release") {
    hold(BTN_LONG_PRESS_MS + 100);

    fakeGpioSet(PIN_CAPTIVE_BTN, false);
    run(BTN_DEBOUNCE_MS / 2);
    fakeGpioSet(PIN_CAPTIVE_BTN, true);
    run(100);

    CHECK(toggleCount == 0);
    CHECK(lastColor() == LIGHT_BLUE);
}

TEST_CASE_FIXTURE(ButtonFixture, "long press of enter on the panel - beep and the portal right away") {
    realOnPanelKey('E', 'H');
    CHECK(toggleCount == 1);

    run(5);
    CHECK(beeps == 1);
}

TEST_CASE_FIXTURE(ButtonFixture, "long press of enter - nothing when the portal can't be toggled") {
    canToggle = false;
    realOnPanelKey('E', 'H');
    run(5);

    CHECK(toggleCount == 0);
    CHECK(beeps == 0);
}

}   // TEST_SUITE
