// core1_main.cpp - where the stream continues after a step, a side change or a new track: seekStreamToCurrentAngle()
// finds the place in the stream where the disk is now. Until it did, the refill sends only the filler: the same offset in
// the other side or track is something else, maybe the middle of a raw run.

#include <vector>
#include <algorithm>
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
extern volatile int streamSide;
extern uint64_t nextRotationStartUs;
extern uint64_t streamResumeUs;
extern uint32_t rotationNo;
extern uint32_t halfTicks[2];
extern bool builtinImageShown;

namespace {

const int SEEK_TIME_MS = 4;                 // the stream continues this far ahead, as the seek takes about that long

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
        streamValid = false;
        streamSide = 0;
        streamResumeUs = 0;
        halfTicks[0] = halfTicks[1] = 0;
    }

    void seekAt(uint32_t msSinceTrackStart) {
        nextRotationStartUs = (fakeNowMs + 200 - msSinceTrackStart) * 1000ull;
        seekStreamToCurrentAngle();
    }

    void refill(void) {
        fillHalfMfmBuffer(FILL_LOWER, fakeNowMs * 1000ull);
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
    CHECK(mfmStream.pos < 170);             // 4 ms = 125 data bytes, plus the header and the mark
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

    refill();
    refill();          // the first one may join to the previous flux
    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "nothing valid in the track (not received yet, other format) - filler") {
    memset(trackData0, 0, READTRACKDATA_SIZE_BYTES);

    seekAt(40);

    CHECK(mfmStream.ended);
    refill();
    refill();
    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "not streaming from an invalid position: filler till the main loop finds it") {
    streamValid = false;
    mfmBuffer[0] = 1;

    refill();

    CHECK(lowerHalfIsFiller());
}

TEST_CASE_FIXTURE(StreamPositionFixture, "side change: the refill stops using the offset of the other side at once, the seek gets the same angle on the new side") {
    seekAt(50);
    REQUIRE(streamValid);
    uint8_t sector = mfmStream.sector;
    refill();
    CHECK_FALSE(lowerHalfIsFiller());

    fakeGpioLevel[PIN_SIDE1] = false;       // side 1 now, main loop didn't notice it yet
    refill();
    CHECK_FALSE(streamValid);
    CHECK(lowerHalfIsFiller());

    seekAt(50);
    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.side == 1);
    CHECK(mfmStream.sector == sector);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "too close to the end of the rotation: no position, the refill starts the track when it comes") {
    seekAt(198);

    CHECK_FALSE(streamValid);
    CHECK(streamResumeUs == 0);

    refill();                               // 2 ms of filler, then the track from its start

    CHECK(streamValid);
    CHECK(mfmStream.pos < 300);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "new rotation while on the other side - starts on the current side") {
    fakeGpioLevel[PIN_SIDE1] = false;
    nextRotationStartUs = fakeNowMs * 1000ull + 100;

    refill();

    CHECK(streamValid);
    CHECK(streamSide == 1);
    CHECK(mfmStream.track == trackData1);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "the time of the found position counts in what the ring still has to play") {
    halfTicks[0] = 1024 * 600;              // 4.096 ms each
    halfTicks[1] = 1024 * 600;

    seekAt(60);

    CHECK(streamValid);
    CHECK(streamResumeUs == fakeNowMs * 1000ull + 4000 + 8192);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "rotations went on while stepping and loading: position in the rotation of now") {
    nextRotationStartUs = (fakeNowMs - 450) * 1000ull;      // 650 ms ago was its start: it's 50 ms into the rotation now

    seekStreamToCurrentAngle();

    CHECK(nextRotationStartUs == (fakeNowMs + 150) * 1000ull);
    CHECK(streamValid);
    CHECK(mfmStream.sector == 3);           // 54 ms
}

TEST_CASE_FIXTURE(StreamPositionFixture, "refill passes the sector being streamed on as posStreamed") {
    posStreamed.sector = 0;
    seekAt(25);
    refill();

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

    nextRotationStartUs = (fakeNowMs + 140) * 1000ull;     // 60 ms into the rotation
    lastStepTime = fakeNowMs;
    core1Loop();                            // stepping
    fakeNowMs += 20;
    core1Loop();                            // tracks loaded, position found

    CHECK(streamValid);
    CHECK(memcmp(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(mfmStream.sector == 5);           // 60 + 20 ms + seek time + 2 halves of filler = 92 ms: sector 5 is 80.5 - 100 ms
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

// the IPC message as core 0 sends it: track #, side #, stream
static void sendToCore1(IpcCommand command, int track, const std::vector<uint8_t>& stream)
{
    IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);
    REQUIRE(bfr != NULL);
    uint32_t length = stream.empty() ? 0 : ((stream[0] << 8) | stream[1]);     // just the stream, as the host sends it
    REQUIRE(2 + length <= READTRACKDATA_SIZE_BYTES);
    bfr->data[0] = track;
    bfr->data[1] = 0;
    if(length > 0) {
        memcpy(bfr->data + 2, stream.data(), length);
    }
    bfr->trackNo = track;
    bfr->sideNo = 0;
    ipcSetBufferAndPutToFifo(bfr, 1, command, (command == CMD_SHOW_REAL_IMAGE || command == CMD_SHOW_BUILTIN_IMAGE) ? 0 : 2 + length);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "built-in floppy: streamed from its own PSRAM area, the real image waits until CMD_SHOW_REAL_IMAGE") {
    ipcInit();
    std::vector<uint8_t> builtin = buildTestTrack(5, 0, 10);
    memset(trackData0, 0, READTRACKDATA_SIZE_BYTES);
    memset(trackData1, 0, READTRACKDATA_SIZE_BYTES);
    hwPosition.track = 5;
    builtinImageShown = true;

    sendToCore1(CMD_STORE_BUILTIN_TRACK, 5, builtin);          // track under the head: streamed right away
    sendToCore1(CMD_STORE_TRACK, 5, side0);                    // real image: PSRAM only
    for(int i=0; i<3; i++) {
        core1Loop();
    }

    CHECK(memcmp(trackData0, builtin.data(), READTRACKDATA_SIZE_BYTES) == 0);
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES);
    psramLoadTrack(5, 0, t.data());
    CHECK(t == side0);
    psramLoadBuiltinTrack(5, 0, t.data());
    CHECK(t == builtin);

    // after a step: from the built-in area, side 1 empty there
    lastStepTime = fakeNowMs;
    core1Loop();
    fakeNowMs += 20;
    core1Loop();
    CHECK(memcmp(trackData0, builtin.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(std::all_of(trackData1, trackData1 + READTRACKDATA_SIZE_BYTES, [](uint8_t b) { return b == 0; }));

    // switch: the real tracks from PSRAM, streamed from where the disk is
    psramStoreTrack(5, 1, side1.data());
    nextRotationStartUs = (fakeNowMs + 140) * 1000ull;
    sendToCore1(CMD_SHOW_REAL_IMAGE, 0, {});
    core1Loop();                            // the command
    core1Loop();                            // tracks loaded
    CHECK_FALSE(builtinImageShown);
    CHECK(memcmp(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(memcmp(trackData1, side1.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(streamValid);
    CHECK(mfmStream.track == trackData0);

    // built-in tracks don't get streamed anymore
    sendToCore1(CMD_STORE_BUILTIN_TRACK, 5, builtin);
    core1Loop();
    CHECK(memcmp(trackData0, side0.data(), READTRACKDATA_SIZE_BYTES) == 0);

    // WiFi setup started: back to the built-in floppy, from its PSRAM area
    sendToCore1(CMD_SHOW_BUILTIN_IMAGE, 0, {});
    core1Loop();
    core1Loop();
    CHECK(builtinImageShown);
    CHECK(memcmp(trackData0, builtin.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(std::all_of(trackData1, trackData1 + READTRACKDATA_SIZE_BYTES, [](uint8_t b) { return b == 0; }));
}

TEST_CASE_FIXTURE(StreamPositionFixture, "the seek takes ms, but the interrupts stay enabled - only the short parts have them off") {
    fakeMillisStep = 0;
    fakeIrqsOffMaxMs = 0;

    seekAt(100);

    CHECK(fakeIrqsOffMaxMs == 0);
    CHECK_FALSE(fakeIrqsDisabled);
}

}
