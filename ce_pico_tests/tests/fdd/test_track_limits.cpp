// Tracks are 0 .. MAX_TRACKS-1. Stepping must stop at the last one, and a track number beyond it must never be stored
// over another track (or beyond the track area) in PSRAM.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "psram.h"

extern TDrivePosition hwPosition;
void floppyStepISR(uint gpio, uint32_t event_mask);    // core1_main.cpp

namespace {

void stepIn(int steps)
{
    fakeGpioLevel[PIN_MOT_EN] = false;      // motor on
    fakeGpioLevel[PIN_DIR] = false;         // L: towards higher tracks

    for (int i = 0; i < steps; i++) {
        fakeNowMs += 3;                     // ST step rate - not filtered out as a glitch
        floppyStepISR(PIN_STEP, GPIO_IRQ_EDGE_FALL);
    }
}

std::vector<uint8_t> psramTrack(int track, int side)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES);
    psramLoadTrack(track, side, t.data());
    return t;
}

}   // namespace

TEST_SUITE("fdd track limits") {

TEST_CASE("stepping in stops at the last track") {
    fakeReset();
    hwPosition.track = MAX_TRACKS - 3;

    stepIn(10);

    CHECK(hwPosition.track == MAX_TRACKS - 1);     // used to reach MAX_TRACKS
}

TEST_CASE("track beyond the last one is not stored to PSRAM") {
    fakeReset();
    std::vector<uint8_t> last(READTRACKDATA_SIZE_BYTES, 0x55);
    psramStoreTrack(MAX_TRACKS - 1, 1, last.data());
    std::vector<uint8_t> before = fakePsram;

    std::vector<uint8_t> bad(READTRACKDATA_SIZE_BYTES, 0xaa);
    psramStoreTrack(MAX_TRACKS, 0, bad.data());
    psramStoreTrack(MAX_TRACKS + 5, 1, bad.data());
    psramStoreSector(MAX_TRACKS + 5, 1, 1000, bad.data(), 500, 100);

    CHECK(fakePsram == before);                    // track 89 not overwritten, nothing beyond the track area written
    CHECK(psramTrack(MAX_TRACKS - 1, 1) == last);
}

TEST_CASE("loading a track beyond the last one reads the last one, not beyond the track area") {
    fakeReset();
    std::vector<uint8_t> last(READTRACKDATA_SIZE_BYTES, 0x55);
    psramStoreTrack(MAX_TRACKS - 1, 0, last.data());

    CHECK(psramTrack(MAX_TRACKS, 0) == last);
}

}
