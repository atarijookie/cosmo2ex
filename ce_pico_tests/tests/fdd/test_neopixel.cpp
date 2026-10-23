// neopixel.cpp - the status LED: the color of each state, sent only when it changes (nothing while the state stays,
// so the main loop doesn't spend time on it), blinks only while there's an error, frames never closer than the
// latch time, and the color sent again after a button press (the button shares the pin).

#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_neopixel.h"

#include "defs.h"
#include "neopixel.h"

namespace {

uint32_t grb(uint8_t r, uint8_t g, uint8_t b)
{
    return (((uint32_t) g) << 16) | (((uint32_t) r) << 8) | b;
}

uint8_t red(uint32_t c)   { return (c >> 8) & 0xff; }
uint8_t green(uint32_t c) { return (c >> 16) & 0xff; }
uint8_t blue(uint32_t c)  { return c & 0xff; }

struct NeopixelFixture {
    NeopixelFixture() {
        fakeReset();
        fakeMillisStep = 0;
        fakeNowMs = 100000;                 // the blink phase is from the clock - start on a whole blink period
        ledSetInitStep(LED_STEP_SETUP);
        neopixelInit();
    }

    // main loop passes, 1 ms apart
    void run(uint32_t ms) {
        for (uint32_t i = 0; i < ms; i++) {
            neopixelProcess();
            fakeNowMs++;
        }
    }

    // to the start of a blink period of this error code - the blink phase is from the clock
    void toPeriodStart(int code) {
        uint32_t period = code * 500 + 1500;
        fakeNowMs += period - (fakeNowMs % period);
    }

    uint32_t last(void) {
        REQUIRE(!fakeNeopixelFrames.empty());
        return fakeNeopixelFrames.back().grb;
    }
};

}   // namespace

TEST_SUITE("fdd neopixel") {

TEST_CASE_FIXTURE(NeopixelFixture, "init - orange, brighter with each step, each step sent once") {
    uint32_t prev = 0;
    for (int step = LED_STEP_SETUP; step <= LED_STEP_COUNT; step++) {
        ledSetInitStep(step);
        size_t before = fakeNeopixelFrames.size();
        run(100);
        CHECK(fakeNeopixelFrames.size() == before + 1);

        uint32_t c = last();
        CHECK(red(c) > green(c));           // orange
        CHECK(green(c) > 0);
        CHECK(blue(c) == 0);
        CHECK(red(c) > red(prev));
        prev = c;
    }
}

TEST_CASE_FIXTURE(NeopixelFixture, "running - nothing sent while the state stays") {
    ledSetRunning(true);
    run(10);
    size_t frames = fakeNeopixelFrames.size();

    for (int i = 0; i < 100; i++) {         // the main loop sets the status on every pass
        ledSetRunning(true);
        run(100);
    }
    CHECK(fakeNeopixelFrames.size() == frames);
}

TEST_CASE_FIXTURE(NeopixelFixture, "connected - green with an image, dim white without one") {
    ledSetRunning(true);
    run(10);
    CHECK(last() == grb(0, green(last()), 0));
    CHECK(green(last()) > 0);

    ledSetRunning(false);
    run(10);
    uint32_t c = last();
    CHECK(red(c) == green(c));
    CHECK(green(c) == blue(c));
    CHECK(red(c) > 0);
    CHECK(red(c) < 16);                     // dim
}

TEST_CASE_FIXTURE(NeopixelFixture, "captive portal - blue") {
    ledSetCaptivePortal();
    run(10);
    CHECK(last() == grb(0, 0, blue(last())));
    CHECK(blue(last()) > 0);
}

TEST_CASE_FIXTURE(NeopixelFixture, "error - red blinks with the code, then a pause, only on and off are sent") {
    run(10);
    toPeriodStart(LED_ERR_NO_WIFI);
    fakeNeopixelFrames.clear();

    ledSetError(LED_ERR_NO_WIFI);           // 3 blinks
    run(2 * (3 * 500 + 1500));              // two periods

    int blinks = 0;
    for (const auto& f : fakeNeopixelFrames) {
        CHECK((f.grb == 0 || f.grb == grb(red(f.grb), 0, 0)));
        blinks += (f.grb != 0) ? 1 : 0;
    }
    CHECK(blinks == 6);
    CHECK(fakeNeopixelFrames.size() == 12); // on + off each - not a frame per pass
}

TEST_CASE_FIXTURE(NeopixelFixture, "frames at least NEOPIXEL_GAP_MS apart - the newest color is sent after the gap") {
    run(10);
    ledSetRunning(true);
    neopixelProcess();
    ledSetCaptivePortal();
    neopixelProcess();                      // same ms - must wait
    ledSetRunning(false);
    neopixelProcess();

    REQUIRE(fakeNeopixelFrames.size() == 2);
    run(NEOPIXEL_GAP_MS + 1);
    REQUIRE(fakeNeopixelFrames.size() == 3);

    CHECK(fakeNeopixelFrames[2].timeMs - fakeNeopixelFrames[1].timeMs >= NEOPIXEL_GAP_MS);
    CHECK(red(last()) == blue(last()));     // the last one asked for (no image), the portal one skipped
}

TEST_CASE_FIXTURE(NeopixelFixture, "resend after a button press, the same color again") {
    ledSetRunning(true);
    run(10);
    size_t frames = fakeNeopixelFrames.size();
    uint32_t c = last();

    neopixelResend();
    run(10);
    CHECK(fakeNeopixelFrames.size() == frames + 1);
    CHECK(last() == c);

    run(100);
    CHECK(fakeNeopixelFrames.size() == frames + 1);   // just once
}

TEST_CASE_FIXTURE(NeopixelFixture, "status from the connection state") {
    auto colorFor = [&](bool ssid, bool wifiFailed, bool wifi, bool hostFound, bool host, bool image) {
        ledSetFromConnection(ssid, wifiFailed, wifi, hostFound, host, image);
        run(10);
        return last();
    };

    uint32_t joining = colorFor(true, false, false, false, false, false);
    uint32_t discovery = colorFor(true, false, true, false, false, false);
    uint32_t connecting = colorFor(true, false, true, true, false, false);
    CHECK(red(joining) < red(discovery));   // orange, brighter as it gets further
    CHECK(red(discovery) < red(connecting));
    CHECK(blue(connecting) == 0);

    uint32_t connected = colorFor(true, false, true, true, true, true);
    CHECK(connected == grb(0, green(connected), 0));                                      // connected, image
    CHECK(blue(colorFor(true, false, true, true, true, false)) > 0);                     // connected, no image - dim white

    // errors blink red - the code is the count of blinks within a period
    auto blinksFor = [&](bool ssid, bool wifiFailed, int code) {     // over one period of the expected code
        toPeriodStart(code);
        fakeNeopixelFrames.clear();
        int blinks = 0;
        for (int i = 0; i < code * 500 + 1500; i++) {
            ledSetFromConnection(ssid, wifiFailed, false, false, false, false);
            size_t before = fakeNeopixelFrames.size();
            run(1);
            if (fakeNeopixelFrames.size() > before && last() != 0) {
                blinks++;
            }
        }
        return blinks;
    };
    CHECK(blinksFor(false, false, LED_ERR_NO_SSID) == LED_ERR_NO_SSID);
    CHECK(blinksFor(true, true, LED_ERR_NO_WIFI) == LED_ERR_NO_WIFI);
}

}
