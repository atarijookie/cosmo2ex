// core1_main.cpp - where the stream continues after a step, a side change or a new track: seekStreamToCurrentAngle()
// finds the place in the stream where the disk is now. Until it did, the refill sends only the filler: the same offset in
// the other side or track is something else, maybe the middle of a raw run.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "core1_main.h"
#include "mfm_stream.h"
#include "mfm_test_track.h"
#include "psram.h"
#include "ipc.h"

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
extern uint16_t mfmBuffer[];
extern TDrivePosition posStreamed, hwPosition;
extern volatile uint32_t lastStepTime;
extern MfmStream mfmStream;
extern volatile bool streamValid;
extern volatile bool trackRestart;
extern volatile int streamSide;
extern volatile uint32_t timeTrackStartUs;

namespace {

const int SEEK_TIME_MS = 3;                 // the stream continues this far ahead, as the seek takes about that long

struct StreamPositionFixture {
    std::vector<uint8_t> side0 = buildTestTrack(5, 0, 9);
    std::vector<uint8_t> side1 = buildTestTrack(5, 1, 9);

    StreamPositionFixture() {
        fakeReset();
        mfmStreamGlobalInit(150000000);
        memcpy(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES);
        memcpy(trackData1, side1.data(), READTRACKDATA_SIZE_BYTES);
        fakeGpioLevel[PIN_SIDE1] = true;        // side 0
        mfmStream = MfmStream();
        trackRestart = false;
        streamValid = false;
        streamSide = 0;
    }

    void seekAt(uint32_t msSinceTrackStart) {
        timeTrackStartUs = (fakeNowMs - msSinceTrackStart) * 1000;
        seekStreamToCurrentAngle();
    }

    bool lowerHalfIsFiller(void) {
        for(int i=0; i<1024; i++) {
            if(mfmBuffer[i] != 599) {
                return false;
            }
        }
        return true;
    }
};

}   // namespace

TEST_SUITE("fdd stream position") {

TEST_CASE_FIXTURE(StreamPositionFixture, "stream continues where the disk is now") {
    for(int sector=1; sector<=9; sector++) {
        CAPTURE(sector);
        seekAt(2 + (sector - 1) * 19.65 + 5 - SEEK_TIME_MS);    // 5 ms into this sector

        CHECK(streamValid);
        CHECK(mfmStream.sector == sector);
        CHECK(mfmStream.side == 0);
        CHECK(mfmStream.trackNo == 5);
    }
}

TEST_CASE_FIXTURE(StreamPositionFixture, "at the very start of the track - before the first sector mark") {
    seekAt(0);
    CHECK(streamValid);
    CHECK(mfmStream.pos < 150);             // 3 ms = 94 data bytes, plus the header and the mark
}

TEST_CASE_FIXTURE(StreamPositionFixture, "side 1 - taken from the side 1 track") {
    fakeGpioLevel[PIN_SIDE1] = false;

    seekAt(40);

    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.track == trackData1);
    CHECK(mfmStream.side == 1);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "past the end of the track data - filler until the track restarts") {
    seekAt(195);

    CHECK(streamValid);
    CHECK(mfmStream.ended);

    fillHalfMfmBuffer(FILL_LOWER);
    fillHalfMfmBuffer(FILL_LOWER);          // the first one may join to the previous flux
    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "nothing valid in the track (not received yet, other format) - filler") {
    memset(trackData0, 0, READTRACKDATA_SIZE_BYTES);

    seekAt(40);

    CHECK(mfmStream.ended);
    fillHalfMfmBuffer(FILL_LOWER);
    fillHalfMfmBuffer(FILL_LOWER);
    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "not streaming from an invalid position: filler till the main loop finds it") {
    streamValid = false;
    mfmBuffer[0] = 1;

    fillHalfMfmBuffer(FILL_LOWER);

    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "side change: the refill stops using the offset of the other side at once, the seek gets the same angle on the new side") {
    seekAt(50);
    REQUIRE(streamValid);
    uint8_t sector = mfmStream.sector;
    fillHalfMfmBuffer(FILL_LOWER);
    CHECK_FALSE(lowerHalfIsFiller());

    fakeGpioLevel[PIN_SIDE1] = false;       // side 1 now, main loop didn't notice it yet
    fillHalfMfmBuffer(FILL_LOWER);
    CHECK_FALSE(streamValid);
    CHECK(lowerHalfIsFiller());

    seekAt(50);
    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.side == 1);
    CHECK(mfmStream.sector == sector);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "new rotation: the refill starts at the track start, not where the seek got") {
    seekAt(150);
    REQUIRE(streamValid);

    trackRestart = true;                    // the rotation alarm came
    fillHalfMfmBuffer(FILL_LOWER);

    CHECK_FALSE(trackRestart);
    CHECK(streamValid);
    CHECK(mfmStream.pos < 300);             // from the start of the track, not from where it was
}

TEST_CASE_FIXTURE(StreamPositionFixture, "track restart while on the other side - starts on the current side") {
    fakeGpioLevel[PIN_SIDE1] = false;
    trackRestart = true;

    fillHalfMfmBuffer(FILL_LOWER);

    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.track == trackData1);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "refill passes the sector being streamed on as posStreamed") {
    posStreamed.sector = 0;
    seekAt(25);
    fillHalfMfmBuffer(FILL_LOWER);

    CHECK(posStreamed.track == 5);
    CHECK(posStreamed.side == 0);
    CHECK(posStreamed.sector >= 1);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "main loop: after a step the track from PSRAM is streamed from where the disk is, side change finds it on the other side") {
    ipcInit();
    psramStoreTrack(5, 0, side0.data());
    psramStoreTrack(5, 1, side1.data());
    memset(trackData0, 0, READTRACKDATA_SIZE_BYTES);
    memset(trackData1, 0, READTRACKDATA_SIZE_BYTES);
    hwPosition.track = 5;

    timeTrackStartUs = (fakeNowMs - 60) * 1000;
    lastStepTime = fakeNowMs;
    core1Loop();                            // stepping
    fakeNowMs += 20;
    core1Loop();                            // tracks loaded, position found

    CHECK(streamValid);
    CHECK(memcmp(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(mfmStream.sector == 5);           // 60 + 20 ms + seek time = 83 ms: sector 5 starts at 1.9 + 4 * 19.65 = 80.5 ms
    CHECK(mfmStream.side == 0);

    fakeGpioLevel[PIN_SIDE1] = false;       // side 1
    core1Loop();

    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.side == 1);
    CHECK(mfmStream.sector == 5);

    fakeGpioLevel[PIN_SIDE1] = true;        // back, for the other tests
    core1Loop();
}

TEST_CASE_FIXTURE(StreamPositionFixture, "the seek takes ms, but the interrupts stay enabled - only the short parts have them off") {
    fakeMillisStep = 0;
    fakeIrqsOffMaxMs = 0;

    seekAt(100);

    CHECK(fakeIrqsOffMaxMs == 0);
    CHECK_FALSE(fakeIrqsDisabled);
}

}
