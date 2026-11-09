// core1_main.cpp - track rotation: INDEX is low for the first 5 ms of each 200 ms rotation, and the track start comes out of
// the output at its falling edge, as on a real drive - protections measure the time from it. The WD1772 also counts INDEX
// pulses (spin-up, timeouts), so they must come even while the core1 main loop is busy for longer than the pulse - loading
// tracks from PSRAM after a step, storing received tracks, debug output.

#include <vector>
#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "core1_main.h"
#include "mfm_stream.h"
#include "mfm_test_track.h"

extern uint64_t nextRotationStartUs;        // core1_main.cpp
extern uint32_t rotationNo;
extern uint64_t streamResumeUs;
extern uint32_t halfTicks[2];
extern volatile bool streamValid;
extern volatile int streamSide;
extern MfmStream mfmStream;
extern uint16_t mfmBuffer[];
extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
uint64_t whenRefillPlays(bool readingUpper, uint32_t intoHalf);

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

TEST_CASE_FIXTURE(RotationFixture, "INDEX is low for the first 5 ms of every 200 ms rotation, without the main loop running") {
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

    CHECK(fell == std::vector<uint32_t>{ 200, 400, 600 });
    CHECK(rose == std::vector<uint32_t>{ 205, 405, 605 });
}

TEST_CASE_FIXTURE(RotationFixture, "the falling edge of INDEX is at the time the refill puts the track start to") {
    uint64_t trackStart = nextRotationStartUs;
    CHECK(trackStart == (start + 200) * 1000);

    sleep_ms(199);
    CHECK(fakeGpioLevel[PIN_INDEX] == true);
    sleep_ms(1);
    CHECK(fakeGpioLevel[PIN_INDEX] == false);
    CHECK(fakeNowMs * 1000 == trackStart);
}

TEST_CASE_FIXTURE(RotationFixture, "rotations don't drift - 50th INDEX edge is exactly at 10 s") {
    uint32_t fallTime = 0;
    bool prev = true;

    for (int ms = 0; ms < 50 * 200; ms++) {
        fakeNowMs++;
        get_absolute_time();            // something reads the clock every ms, alarms fire on time
        if (prev && !fakeGpioLevel[PIN_INDEX]) fallTime = fakeNowMs;
        prev = fakeGpioLevel[PIN_INDEX];
    }

    CHECK(fallTime == start + 50 * 200);
}

TEST_CASE_FIXTURE(RotationFixture, "INDEX edge that came while interrupts were disabled is done when they are enabled again, the next one stays on the grid") {
    sleep_ms(199);
    uint32_t irqState = save_and_disable_interrupts();
    sleep_ms(3);                        // 202 ms - falling edge due, but the IRQ waits
    CHECK(fakeGpioLevel[PIN_INDEX] == true);
    restore_interrupts(irqState);
    CHECK(fakeGpioLevel[PIN_INDEX] == false);

    sleep_ms(3);                        // 205 ms
    CHECK(fakeGpioLevel[PIN_INDEX] == true);

    while (fakeGpioLevel[PIN_INDEX]) {
        sleep_ms(1);
    }
    CHECK(sinceStart() == 400);
}

TEST_CASE("the refill has lower priority than the INDEX alarm, so the edge isn't late because of it") {
    fakeReset();
    setupDmaToPwm();
    CHECK(fakeIrqPriority[DMA_IRQ_0] > PICO_DEFAULT_IRQ_PRIORITY);
}

}

TEST_SUITE("fdd track start in the output") {

namespace {

const uint64_t T0 = 5000000;            // the time the test starts at
const int TICKS_PER_US = 150;

struct OutputFixture {
    std::vector<uint8_t> side0 = buildTestTrack(5, 0, 9);
    std::vector<uint8_t> side1 = buildTestTrack(5, 1, 9);

    OutputFixture() {
        fakeReset();
        mfmStreamGlobalInit(150000000);
        memcpy(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES);
        memcpy(trackData1, side1.data(), READTRACKDATA_SIZE_BYTES);
        fakeGpioLevel[PIN_SIDE1] = true;
        mfmStream = MfmStream();
        streamSide = 0;
        streamValid = false;
        streamResumeUs = 0;
        rotationNo = 0;
    }

    std::vector<uint16_t> half(void) {
        return std::vector<uint16_t>(mfmBuffer, mfmBuffer + 1024);
    }

    // the intervals of a track from its start, as a stream of its own makes them
    std::vector<uint16_t> trackStart(const std::vector<uint8_t>& track, int count) {
        MfmStream s = MfmStream();
        mfmStreamStart(&s, track.data(), track.size());
        std::vector<uint16_t> out(count);
        mfmStreamFill(&s, out.data(), count);
        return out;
    }

    uint64_t ticksOf(const std::vector<uint16_t>& intervals, size_t count) {
        uint64_t ticks = 0;
        for(size_t i=0; i<count; i++) {
            ticks += intervals[i] + 1;
        }
        return ticks;
    }
};

}   // namespace

TEST_CASE_FIXTURE(OutputFixture, "the track starts in the output exactly at the track start time, inside of a half") {
    nextRotationStartUs = T0 + 2000;    // 2 ms into this half
    streamValid = false;

    fillHalfMfmBuffer(FILL_LOWER, T0);

    std::vector<uint16_t> out = half();
    std::vector<uint16_t> expected = trackStart(side0, 100);

    size_t fillerCount = 0;             // what's before the track start
    while(out[fillerCount] == 599) {
        fillerCount++;
    }
    fillerCount = 2000 * TICKS_PER_US / 600 + ((2000 * TICKS_PER_US % 600) ? 1 : 0);

    uint64_t ticksBefore = ticksOf(out, fillerCount);
    CHECK(ticksBefore >= 2000 * TICKS_PER_US);
    CHECK(ticksBefore < 2000 * TICKS_PER_US + 600);     // within one interval: 4 us

    CHECK(std::vector<uint16_t>(out.begin() + fillerCount, out.begin() + fillerCount + 100) == expected);

    CHECK(streamValid);
    CHECK(nextRotationStartUs == T0 + 2000 + 200000);
    CHECK(rotationNo == 1);
}

TEST_CASE_FIXTURE(OutputFixture, "the track starts at the grid in every rotation, no drift over many, with cell times changing the lengths") {
    // long tracks don't play exactly in the 200 ms: what's left is cut, or filled by the filler, the next one starts on time
    std::vector<uint8_t> t = {0, 0, MFM_STREAM_VERSION, 0, MFM_ESC, MFM_OP_CELL_TIME, 0x7d, 0x7b};   // a bit shorter than 2 us
    uint32_t rnd = 11;
    for(int i=0; i<7000; i++) {         // longer than a rotation
        rnd = rnd * 1103515245 + 12345;
        uint8_t val = (rnd >> 16) & 0xff;
        t.push_back((val == MFM_ESC) ? 0x12 : val);
    }
    t.push_back(MFM_ESC);
    t.push_back(MFM_OP_END);
    t[0] = t.size() >> 8;
    t[1] = t.size() & 0xff;
    t.resize(READTRACKDATA_SIZE_BYTES, 0);
    memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);

    nextRotationStartUs = T0 + 1000;
    uint64_t playTicks = 0;             // time of what was put to the output so far
    uint32_t lastRotation = rotationNo;
    int rotations = 0;

    while(rotations < 50) {
        uint64_t rotationBefore = nextRotationStartUs;
        fillHalfMfmBuffer(FILL_LOWER, T0 + playTicks / TICKS_PER_US);

        if(rotationNo != lastRotation) {
            lastRotation = rotationNo;
            rotations++;

            CHECK(nextRotationStartUs == rotationBefore + 200000);

            // the first interval of the track is in this half: the time up to it is on the grid
            std::vector<uint16_t> out = half();
            uint64_t want = (rotationBefore - T0) * TICKS_PER_US - playTicks;
            uint64_t ticks = 0;
            size_t i = 0;
            while(ticks < want) {
                ticks += out[i++] + 1;
            }
            CHECK(ticks - want < 8 * TICKS_PER_US + 200);   // an interval at most
        }

        playTicks += halfTicks[0];
    }

    CHECK(nextRotationStartUs == T0 + 1000 + 50 * 200000ull);
}

TEST_CASE_FIXTURE(OutputFixture, "the half remembers how long it plays") {
    nextRotationStartUs = T0 + 100000;
    streamValid = false;
    fillHalfMfmBuffer(FILL_UPPER, T0);
    CHECK(halfTicks[1] == 1024 * 600);

    clearMfmBuffer();
    CHECK(halfTicks[0] == 1024 * 600);
    CHECK(halfTicks[1] == 1024 * 600);
}

TEST_CASE_FIXTURE(OutputFixture, "when the refill plays: after the rest of the half being played, from the time now") {
    halfTicks[0] = 1024 * 600;          // 4.096 ms
    halfTicks[1] = 1024 * 1200;         // 8.192 ms

    CHECK(whenRefillPlays(false, 0) == fakeNowMs * 1000ull + 4096);
    CHECK(whenRefillPlays(true, 0) == fakeNowMs * 1000ull + 8192);
    CHECK(whenRefillPlays(false, 512) == fakeNowMs * 1000ull + 2048);    // IRQ late
}

TEST_CASE_FIXTURE(OutputFixture, "refill a bit after the track start: starts the track late by just that, no lost rotation") {
    nextRotationStartUs = T0 - 10;

    fillHalfMfmBuffer(FILL_LOWER, T0);

    CHECK(streamValid);
    CHECK(rotationNo == 1);
    CHECK(nextRotationStartUs == T0 - 10 + 200000);

    std::vector<uint16_t> out = half();
    CHECK(std::vector<uint16_t>(out.begin(), out.begin() + 100) == trackStart(side0, 100));
}

TEST_CASE_FIXTURE(OutputFixture, "refills blocked for long (flash write): the position is lost, the main loop finds it, the grid stays") {
    mfmStreamStart(&mfmStream, trackData0, READTRACKDATA_SIZE_BYTES);
    streamValid = true;
    nextRotationStartUs = T0;

    fillHalfMfmBuffer(FILL_LOWER, T0 + 450000);     // more than 2 rotations

    CHECK_FALSE(streamValid);
    CHECK(nextRotationStartUs == T0 + 600000);
    for(uint16_t top : half()) {
        CHECK(top == 599);
    }
}

TEST_CASE_FIXTURE(OutputFixture, "new rotation takes the side selected now") {
    fakeGpioLevel[PIN_SIDE1] = false;
    nextRotationStartUs = T0 + 100;

    fillHalfMfmBuffer(FILL_LOWER, T0);

    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.track == trackData1);
}

TEST_CASE_FIXTURE(OutputFixture, "position found by the seek, its time is after this half: the next refill goes on waiting") {
    MfmStream expected = MfmStream();
    mfmStreamStart(&expected, trackData0, READTRACKDATA_SIZE_BYTES);
    mfmStreamSeek(&expected, 50000);
    uint16_t expectedOut[100];
    MfmStream copy = expected;
    mfmStreamFill(&copy, expectedOut, 100);

    mfmStream = expected;
    streamValid = true;
    streamSide = 0;
    nextRotationStartUs = T0 + 150000;
    streamResumeUs = T0 + 6000;

    fillHalfMfmBuffer(FILL_LOWER, T0);          // 4096 us of filler
    for(uint16_t top : half()) {
        CHECK(top == 599);
    }
    CHECK(streamResumeUs == T0 + 6000);

    fillHalfMfmBuffer(FILL_LOWER, T0 + 4096);   // 1904 us of filler, then the stream

    std::vector<uint16_t> out = half();
    size_t fillerCount = 1904 * TICKS_PER_US / 600;
    CHECK(out[fillerCount - 1] == 599);
    CHECK(std::vector<uint16_t>(out.begin() + fillerCount, out.begin() + fillerCount + 100) == std::vector<uint16_t>(expectedOut, expectedOut + 100));
    CHECK(streamResumeUs == 0);
}

TEST_CASE_FIXTURE(OutputFixture, "position found by the seek: filler up to its time, then the stream from there") {
    MfmStream expected = MfmStream();
    mfmStreamStart(&expected, trackData0, READTRACKDATA_SIZE_BYTES);
    mfmStreamSeek(&expected, 50000);
    uint16_t expectedOut[100];
    MfmStream copy = expected;
    mfmStreamFill(&copy, expectedOut, 100);

    mfmStream = expected;
    streamValid = true;
    streamSide = 0;
    nextRotationStartUs = T0 + 150000;
    streamResumeUs = T0 + 3000;

    fillHalfMfmBuffer(FILL_LOWER, T0);

    std::vector<uint16_t> out = half();
    size_t fillerCount = 3000 * TICKS_PER_US / 600;
    CHECK(out[fillerCount - 1] == 599);
    CHECK(std::vector<uint16_t>(out.begin() + fillerCount, out.begin() + fillerCount + 100) == std::vector<uint16_t>(expectedOut, expectedOut + 100));
    CHECK(streamResumeUs == 0);
}

}
