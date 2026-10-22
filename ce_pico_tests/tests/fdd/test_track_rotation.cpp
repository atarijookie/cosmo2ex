// core1_main.cpp - track rotation: every 200 ms the stream restarts at the track start, INDEX is low for the last 5 ms of
// each rotation. The WD1772 counts INDEX pulses (spin-up, timeouts), so they must come even while the core1 main loop is
// busy for longer than the pulse - loading tracks from PSRAM after a step, storing received tracks, debug output.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "core1_main.h"

extern uint32_t dataIndexInTrack;           // core1_main.cpp - read index in the streamed track
extern volatile uint32_t timeTrackStart;

namespace {

struct RotationFixture {
    uint32_t start;

    RotationFixture() {
        fakeReset();
        start = fakeNowMs;
        trackRotationSetup();
    }

    uint32_t sinceStart(void) {
        return fakeNowMs - start;
    }
};

}   // namespace

TEST_SUITE("fdd track rotation") {

TEST_CASE_FIXTURE(RotationFixture, "INDEX is low for the last 5 ms of every 200 ms rotation, without the main loop running") {
    // the main loop never runs here - only the time goes on, as when it's stuck in a long PSRAM transfer
    std::vector<uint32_t> fell, rose;
    bool prev = fakeGpioLevel[PIN_INDEX];
    CHECK(prev == true);

    for (int ms = 0; ms < 3 * 200 + 10; ms++) {
        sleep_ms(1);
        bool now = fakeGpioLevel[PIN_INDEX];
        if (prev && !now) fell.push_back(sinceStart());
        if (!prev && now) rose.push_back(sinceStart());
        prev = now;
    }

    CHECK(fell == std::vector<uint32_t>{ 195, 395, 595 });
    CHECK(rose == std::vector<uint32_t>{ 200, 400, 600 });
}

TEST_CASE_FIXTURE(RotationFixture, "stream restarts at the track start when INDEX goes high") {
    sleep_ms(100);
    dataIndexInTrack = 5000;            // somewhere in the middle of the track

    sleep_ms(99);                       // 199 ms - INDEX pulse, still the same rotation
    CHECK(dataIndexInTrack == 5000);
    CHECK(fakeGpioLevel[PIN_INDEX] == false);

    sleep_ms(1);                        // 200 ms - next rotation
    CHECK(dataIndexInTrack == STREAM_START_OFFSET);
    CHECK(timeTrackStart == start + 200);
    CHECK(fakeGpioLevel[PIN_INDEX] == true);
}

TEST_CASE_FIXTURE(RotationFixture, "rotations don't drift - 50th rotation starts exactly at 10 s") {
    for (int ms = 0; ms < 50 * 200; ms++) {
        get_absolute_time();            // something reads the clock every ms, alarms fire on time
        fakeNowMs++;
    }
    get_absolute_time();

    CHECK(timeTrackStart == start + 50 * 200);
}

TEST_CASE_FIXTURE(RotationFixture, "track restart that came while interrupts were disabled is handled when they are enabled again") {
    sleep_ms(198);
    uint32_t irqState = save_and_disable_interrupts();
    sleep_ms(3);                        // 201 ms - restart due, but the IRQ waits
    CHECK(fakeGpioLevel[PIN_INDEX] == false);
    restore_interrupts(irqState);

    CHECK(fakeGpioLevel[PIN_INDEX] == true);
    CHECK(dataIndexInTrack == STREAM_START_OFFSET);

    for (int ms = 0; ms < 195; ms++) {  // next INDEX pulse still on the 200 ms grid, not shifted by the late IRQ
        sleep_ms(1);
        if (!fakeGpioLevel[PIN_INDEX]) break;
    }
    CHECK(sinceStart() == 395);
}

}
