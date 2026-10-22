// core1_main.cpp - where the stream continues after a step (updateStreamPositionByFloppyPosition): at the sector the
// disk would be at by now, taken from the stream table of the side being streamed. A missing sector or a bad offset must
// not start the stream in the stream table (its bytes would go out as MFM data), and the stream fill
// (getMfmDataToBuffer) must never read beyond the track buffer.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "utils.h"

extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
extern uint8_t imgSectorsPerTrack;
extern uint32_t dataIndexInTrack;
extern volatile uint32_t timeTrackStart;
extern TDrivePosition posStreamed;

void updateStreamPositionByFloppyPosition(void);        // core1_main.cpp
void getMfmDataToBuffer(uint8_t* bfr, int len);

namespace {

const int NOTHING_TO_STREAM = READTRACKDATA_SIZE_BYTES - 1;

// stream table only: sector s starts at base + s * 1000 (the stream itself doesn't matter for picking the position)
void storeTable(uint8_t* track, int spt, int base)
{
    for (int s = 1; s <= spt; s++) {
        storeWord(track + 2 * s, base + s * 1000);
    }
}

struct StreamPositionFixture {
    StreamPositionFixture() {
        fakeReset();
        memset(trackData0, 0, sizeof(trackData0));
        memset(trackData1, 0, sizeof(trackData1));
        imgSectorsPerTrack = 9;
        fakeGpioLevel[PIN_SIDE1] = true;        // side 0
        dataIndexInTrack = 12345;
    }

    void stepDoneAt(uint32_t msSinceTrackStart) {
        timeTrackStart = fakeNowMs - msSinceTrackStart;
        updateStreamPositionByFloppyPosition();
    }
};

}   // namespace

TEST_SUITE("fdd stream position after step") {

TEST_CASE_FIXTURE(StreamPositionFixture, "stream continues at the sector the disk is at now") {
    storeTable(trackData0, 9, 100);

    stepDoneAt(40);                             // 19 ms per sector - in the 3rd sector

    CHECK(dataIndexInTrack == 100 + 3 * 1000);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "side 1 - offset taken from the side 1 stream table") {
    storeTable(trackData0, 9, 100);
    storeTable(trackData1, 9, 200);             // side 1 laid out differently
    fakeGpioLevel[PIN_SIDE1] = false;           // side 1

    stepDoneAt(40);

    CHECK(dataIndexInTrack == 200 + 3 * 1000);  // used to be side 0's
}

TEST_CASE_FIXTURE(StreamPositionFixture, "sector not in the track (offset 0) - nothing streamed, not the stream table") {
    storeTable(trackData0, 2, 100);             // track has just 2 sectors, the image says 9

    stepDoneAt(40);

    CHECK(dataIndexInTrack == NOTHING_TO_STREAM);

    std::vector<uint8_t> out(64);
    getMfmDataToBuffer(out.data(), out.size());
    CHECK(out == std::vector<uint8_t>(64, 0x55));    // just the filler, until the track restarts
}

TEST_CASE_FIXTURE(StreamPositionFixture, "offset pointing into the stream table or beyond the track buffer - nothing streamed") {
    int offset = 0;
    SUBCASE("into the table")       { offset = 2 * STREAM_TABLE_ITEMS - 1; }
    SUBCASE("beyond the buffer")    { offset = READTRACKDATA_SIZE_BYTES; }

    storeWord(trackData0 + 2 * 3, offset);

    stepDoneAt(40);

    CHECK(dataIndexInTrack == NOTHING_TO_STREAM);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "no sectors per track known - not the end of stream offset in entry 0") {
    storeTable(trackData0, 9, 100);
    storeWord(trackData0, 9000);                // entry 0: end of stream
    imgSectorsPerTrack = 0;

    stepDoneAt(40);

    CHECK(dataIndexInTrack == 100 + 1 * 1000);  // sector 1
}

TEST_CASE_FIXTURE(StreamPositionFixture, "too late in the track for a whole sector, or past its end - nothing streamed until restart") {
    storeTable(trackData0, 9, 100);
    uint32_t ms = 0;
    SUBCASE("last 19 ms")           { ms = 185; }
    SUBCASE("restart overdue")      { ms = 230; }

    stepDoneAt(ms);

    CHECK(dataIndexInTrack == NOTHING_TO_STREAM);

    std::vector<uint8_t> out(64);
    getMfmDataToBuffer(out.data(), out.size());
    CHECK(out == std::vector<uint8_t>(64, 0x55));
    CHECK(dataIndexInTrack <= NOTHING_TO_STREAM);
}

TEST_CASE_FIXTURE(StreamPositionFixture, "stream fill never reads beyond the track buffer") {
    SUBCASE("index beyond the buffer") {
        dataIndexInTrack = READTRACKDATA_SIZE_BYTES + 100;
    }

    SUBCASE("sector mark cut off by the buffer end") {
        dataIndexInTrack = READTRACKDATA_SIZE_BYTES - 4;
        trackData0[READTRACKDATA_SIZE_BYTES - 4] = 0x55;
        trackData0[READTRACKDATA_SIZE_BYTES - 3] = CMD_CURRENT_SECTOR;     // side, track, sector would be beyond the end
        trackData0[READTRACKDATA_SIZE_BYTES - 2] = 0;
        posStreamed.sector = 7;
    }

    std::vector<uint8_t> out(64);
    getMfmDataToBuffer(out.data(), out.size());

    CHECK(dataIndexInTrack <= NOTHING_TO_STREAM);
    CHECK(posStreamed.sector != 0);             // not overwritten from beyond the mark
}

}
