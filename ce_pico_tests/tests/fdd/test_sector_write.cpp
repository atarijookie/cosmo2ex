// core1_main.cpp + mfm_write.cpp - written sector capture: the bytes captured between write start and write end must be
// stored locally (track buffer in RAM + PSRAM) into the sector the ST was writing, and sent to core0 for the host.
//
// The track buffers hold the encoded track format of ce_pico_fdd/encoded_track_format.txt (as produced by the CE server).
// The ST writes a sector right after reading its header, so the write belongs to the sector whose sector mark
// the stream passed last when the write started. The stream is read ahead into the DMA ring, so by the time the write
// ends, posStreamed already holds the next sector - the tests reproduce that by streaming on between write start and end.

#include <vector>
#include <cstdint>
#include <algorithm>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "psram.h"
#include "mfm_write.h"
#include "core1_main.h"
#include "mfm_stream.h"
#include "mfm_test_track.h"

// firmware globals / functions not in the firmware headers
extern TDrivePosition posStreamed;
extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
extern uint8_t imgSectorsPerTrack;

extern volatile bool trackRestart;                      // core1_main.cpp - the next refill starts at the track start
extern volatile bool streamValid;
extern volatile IPCbuffer* wrBuffer;                    // core1_main.cpp - buffer the write IRQ appends to

extern volatile uint8_t wrStreamByte;                   // mfm_write.cpp - byte packing of the captured symbols
extern volatile uint8_t wrBits;

extern IPCbuffer buffersForCore0[IPC_BUFFER_SMALL_COUNT];  // ipc.cpp - write buffers going to core0
extern volatile uint32_t wrWritesDropped;               // core1_main.cpp - writes not captured

namespace {

const int READ_AHEAD_MIN_BYTES = 256;       // stream read ahead of the output: at least one DMA ring half, in captured symbol bytes (4 intervals)

std::vector<uint8_t> buildEncodedTrack(int track, int side, int spt)
{
    return buildTestTrack(track, side, spt);
}

// The bytes the ST writes: a real sector, as it's decoded and stored. Its size follows from the data, about 800 B.
std::vector<uint8_t> writtenBytes(int count, uint8_t seed = 100)
{
    return testWriteSymbols(testSectorRun(testSectorData(seed)));
}

// Pulse intervals as mfm_write.pio captures them (loop counts): 23 / 43 / 63 for 4 / 6 / 8 us, index = MFM_4US / MFM_6US / MFM_8US.
const uint32_t PIO_TICKS[4]  = { 0, 23, 43, 63 };
const uint32_t PIO_TICKS_GAP = 100000;      // first interval of a write - since the last pulse of the previous write, rejected as too long

bool holdWriteIrq;      // true: completed DMA blocks don't get the IRQ handled yet (as when core1 is busy in another handler)

// the PIO pushes one pulse interval, the DMA moves it into the ring, the write IRQ is handled when a DMA block completes
void pioPushPulse(uint32_t ticks)
{
    bool blockDone = fakeDmaTransfer(fakeDmaLastClaimed, 0xffffffff - ticks);   // the PIO counts X down from 0xffffffff

    if (blockDone && !holdWriteIrq) {
        fakeFireIrq(DMA_IRQ_1);
    }
}

void pioPushSymbol(uint8_t symbol)
{
    pioPushPulse(PIO_TICKS[symbol]);
}

// one captured byte = 4 symbols, highest bits first
void captureByte(uint8_t b)
{
    for (int i = 0; i < 4; i++) {
        pioPushSymbol((b >> (6 - 2 * i)) & 3);
    }
}

void captureBytes(const std::vector<uint8_t>& data)
{
    for (uint8_t b : data) {
        captureByte(b);
    }
}

// the delayed write IRQ finally gets handled
void releaseWriteIrq(void)
{
    holdWriteIrq = false;
    fakeFireIrq(DMA_IRQ_1);
}

// WGATE edges - the WGATE IRQ starts / ends the write
void writeGateLow(void)
{
    fakeGpioSet(PIN_WGATE, false);
}

// write end, then the core1 main loop runs - it stores the write locally and passes it to core0
void writeEnd(void)
{
    fakeGpioSet(PIN_WGATE, true);
    handleWriteBuffers();
}

int freeCore0Buffers(void)
{
    int n = 0;
    for (int i = 0; i < IPC_BUFFER_SMALL_COUNT; i++) {
        n += buffersForCore0[i].free ? 1 : 0;
    }
    return n;
}

uint32_t wrBufferLength(void)
{
    REQUIRE(wrBuffer != NULL);
    return wrBuffer->length;
}

// takes the next written sector message for the host from the core0 fifo, returns the written data (beyond the 12 B header)
std::vector<uint8_t> takeWrittenData(void)
{
    IPCbuffer* msg = ipcGetBufferFromFifo(0);
    REQUIRE(msg != NULL);

    uint32_t length = msg->length;
    REQUIRE(length >= 12);
    std::vector<uint8_t> data(msg->data + 12, msg->data + length);

    msg->free = true;
    return data;
}

// read the stream on, as the DMA refills do while the ST is writing - this moves posStreamed. 'bytes': of captured symbols, 4 intervals each.
void streamOn(int bytes)
{
    for(int done = 0; done < bytes; done += READ_AHEAD_MIN_BYTES) {
        fillHalfMfmBuffer(FILL_LOWER);
    }
}

// the ST reads the header of this sector now
void streamToSector(int sector)
{
    trackRestart = true;

    for(int i = 0; i < 100 && (i == 0 || posStreamed.sector != sector); i++) {
        fillHalfMfmBuffer(FILL_LOWER);
    }

    REQUIRE(posStreamed.sector == sector);
}

std::vector<uint8_t> psramTrack(int track, int side)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES);
    psramLoadTrack(track, side, t.data());
    return t;
}

// what the track must look like after the captured write 'data' was written into 'sector': the same, just the data in its raw run
std::vector<uint8_t> expectedAfterWrite(std::vector<uint8_t> t, int sector, const std::vector<uint8_t>& data)
{
    uint8_t run[MFM_SECTOR_RUN_SIZE];
    REQUIRE(mfmDecodeWrittenSector(data.data(), data.size(), run) == MFM_DECODE_SECTOR);

    int offset = testFindRun(t, sector);
    REQUIRE(offset > 0);
    memcpy(&t[offset], run, MFM_SECTOR_RUN_SIZE);

    return t;
}

struct WriteFixture {
    WriteFixture() {
        fakeReset();
        ipcInit();

        wrBuffer = NULL;
        wrBits = 0;
        wrStreamByte = 0;
        holdWriteIrq = false;
        fakeGpioLevel[PIN_WGATE] = true;        // not writing
        writeCaptureSetup();                    // write DMA + its IRQ, buffers prepared, WGATE IRQ

        fakeGpioLevel[PIN_DRIVE_SEL] = false;   // our drive selected (active low)

        mfmStreamGlobalInit(150000000);
        imgSectorsPerTrack = 9;
        memset(trackData0, 0, sizeof(trackData0));
        memset(trackData1, 0, sizeof(trackData1));
        memset((void*) &posStreamed, 0, sizeof(posStreamed));
    }

    // load the track to the streaming buffer + PSRAM, select the side, stream up to and including the header of 'sector'
    void insertTrack(int track, int side, const std::vector<uint8_t>& t, int sector) {
        uint8_t* buf = (side == 0) ? trackData0 : trackData1;
        memcpy(buf, t.data(), READTRACKDATA_SIZE_BYTES);
        psramStoreTrack(track, side, buf);

        fakeGpioLevel[PIN_SIDE1] = (side == 0);         // SIDE1 is H for side 0

        streamToSector(sector);                         // the ST reads the sector header...
        REQUIRE(posStreamed.track == track);
    }

    // ...and writes the data part. Meanwhile the stream moves on by the written length (same speed as the ST writes)
    // plus the read-ahead into the DMA ring. ourDrive = false: the write is for the other drive on the bus, must not be captured.
    void writeSector(const std::vector<uint8_t>& data, bool ourDrive = true) {
        writeGateLow();
        REQUIRE((wrBuffer != NULL) == ourDrive);

        pioPushPulse(PIO_TICKS_GAP);
        captureBytes(data);

        streamOn(data.size() + READ_AHEAD_MIN_BYTES);

        writeEnd();
    }
};

}   // namespace

TEST_SUITE("fdd sector write") {

TEST_CASE_FIXTURE(WriteFixture, "the stream read-ahead reaches the next sector header before the write ends") {
    // the premise of the tests below: a 512 B sector write is about 700 captured bytes long, with the read-ahead the
    // stream passes the end of the data part and the next sector header before WGATE goes high
    insertTrack(5, 0, buildEncodedTrack(5, 0, 9), 3);

    writeGateLow();
    streamOn(700 + READ_AHEAD_MIN_BYTES);

    CHECK(posStreamed.sector == 4);
    writeEnd();
}

TEST_CASE_FIXTURE(WriteFixture, "written data is stored into the sector being written, not into the next one the stream read ahead to") {
    const int track = 5, side = 0, sector = 3;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data = writtenBytes(700);

    insertTrack(track, side, t, sector);
    writeSector(data);
    REQUIRE(posStreamed.sector == sector + 1);          // stream is already in the next sector

    std::vector<uint8_t> expected = expectedAfterWrite(t, sector, data);

    SUBCASE("track buffer in RAM") {
        CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
        CHECK(trackData0[testFindRun(t, sector + 1) + 1] == testSectorData(sector + 1)[0]);     // next sector untouched
        CHECK(trackData0[testFindRun(t, sector) + 1] == testSectorData(100)[0]);
    }

    SUBCASE("track in PSRAM") {
        CHECK(psramTrack(track, side) == expected);
    }

    SUBCASE("message for the host carries the sector being written") {
        IPCbuffer* msg = ipcGetBufferFromFifo(0);
        REQUIRE(msg != NULL);

        IpcCommand command = msg->command;              // IPCbuffer is packed - copy fields before CHECK binds them
        uint32_t length = msg->length;
        uint8_t sectorNo = msg->sectorNo;

        CHECK(command == CMD_SEND_WRITTEN_SECTOR);
        CHECK(getDword(msg->data) == SYNC_TAG_FDD);
        CHECK(getWord(msg->data + 4) == ATN_SECTOR_WRITTEN);
        CHECK(msg->data[10] == track);                  // side 0: bit 7 clear
        CHECK(msg->data[11] == sector);
        CHECK(sectorNo == sector);
        CHECK(length == 12 + data.size());
        CHECK(memcmp(msg->data + 12, data.data(), data.size()) == 0);

        CHECK(ipcGetBufferFromFifo(0) == NULL);
    }
}

TEST_CASE_FIXTURE(WriteFixture, "write on side 1 goes to side 1 of the track, side bit set for the host") {
    const int track = 80, side = 1, sector = 2;        // track 80 == 0x50 == MFM_OP_RAW, must not confuse the parsing
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data = writtenBytes(690);

    insertTrack(track, side, t, sector);
    std::vector<uint8_t> side0Before(trackData0, trackData0 + READTRACKDATA_SIZE_BYTES);

    writeSector(data);
    REQUIRE(posStreamed.sector == sector + 1);

    std::vector<uint8_t> expected = expectedAfterWrite(t, sector, data);
    CHECK(memcmp(trackData1, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == expected);
    CHECK(memcmp(trackData0, side0Before.data(), READTRACKDATA_SIZE_BYTES) == 0);   // other side untouched

    IPCbuffer* msg = ipcGetBufferFromFifo(0);
    REQUIRE(msg != NULL);
    CHECK(msg->data[10] == (0x80 | track));
    CHECK(msg->data[11] == sector);
}

TEST_CASE_FIXTURE(WriteFixture, "write to the last sector of the track, where the stream has no next sector to read ahead to") {
    const int track = 10, side = 0, sector = 9;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data = writtenBytes(700);

    insertTrack(track, side, t, sector);
    writeSector(data);
    REQUIRE(posStreamed.sector == sector);

    std::vector<uint8_t> expected = expectedAfterWrite(t, sector, data);
    CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == expected);
}

TEST_CASE_FIXTURE(WriteFixture, "consecutive writes of two sectors each land in their own sector") {
    // multi-sector write: sector 4 is written right after sector 3, the stream read-ahead runs one sector ahead all the time
    const int track = 7, side = 0;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data3 = writtenBytes(700);
    std::vector<uint8_t> data4 = writtenBytes(680, 101);

    insertTrack(track, side, t, 3);
    writeSector(data3);
    REQUIRE(posStreamed.sector == 4);

    streamToSector(4);                                  // the ST reads sector 4 header now

    writeSector(data4);
    REQUIRE(posStreamed.sector == 5);

    std::vector<uint8_t> expected = expectedAfterWrite(expectedAfterWrite(t, 3, data3), 4, data4);
    CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == expected);

    IPCbuffer* msg1 = ipcGetBufferFromFifo(0);
    IPCbuffer* msg2 = ipcGetBufferFromFifo(0);
    REQUIRE(msg1 != NULL);
    REQUIRE(msg2 != NULL);
    CHECK(msg1->data[11] == 3);
    CHECK(msg2->data[11] == 4);
}

TEST_CASE_FIXTURE(WriteFixture, "write while our drive is not selected is for the other drive and is ignored") {
    // WGATE / WDATA are shared on the ST floppy bus, a write to drive B toggles them for us too
    const int track = 5, side = 0, sector = 3;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);

    insertTrack(track, side, t, sector);
    fakeGpioLevel[PIN_DRIVE_SEL] = true;                // our drive deselected, the ST selected the other one
    int freeBefore = freeCore0Buffers();

    writeSector(writtenBytes(700), false);

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == t);
    CHECK(ipcGetBufferFromFifo(0) == NULL);             // nothing sent to the host
    CHECK(freeCore0Buffers() == freeBefore);            // no write buffer taken
}

TEST_CASE_FIXTURE(WriteFixture, "write to the other drive does not disturb the next write to our drive") {
    const int track = 5, side = 0;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data = writtenBytes(700);

    insertTrack(track, side, t, 3);
    fakeGpioLevel[PIN_DRIVE_SEL] = true;                // write to drive B
    writeSector(std::vector<uint8_t>(700, 0xb6), false);

    fakeGpioLevel[PIN_DRIVE_SEL] = false;               // back to our drive, write sector 4
    streamToSector(4);
    writeSector(data);

    std::vector<uint8_t> expected = expectedAfterWrite(t, 4, data);
    CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == expected);

    IPCbuffer* msg = ipcGetBufferFromFifo(0);           // only our write goes to the host, without drive B data in it
    REQUIRE(msg != NULL);
    uint32_t length = msg->length;
    CHECK(msg->data[11] == 4);
    CHECK(length == 12 + data.size());
    CHECK(memcmp(msg->data + 12, data.data(), data.size()) == 0);
    CHECK(ipcGetBufferFromFifo(0) == NULL);
}

}

TEST_SUITE("fdd write capture") {

// written bytes - pulses come one by one in the tests below, so a byte = 4 symbols, no stream / track needed
const std::vector<uint8_t> WRITTEN = { 0x6b, 0xe7, 0x9d, 0xb6, 0x5a, 0xa5, 0x7e, 0xdb, 0x66, 0xbd };

TEST_CASE_FIXTURE(WriteFixture, "write IRQ has higher priority than the read stream refill IRQ") {
    CHECK(fakeIrqPriority[DMA_IRQ_1] < PICO_DEFAULT_IRQ_PRIORITY);
    CHECK(fakeIrqPriority[DMA_IRQ_1] < fakeIrqPriority[DMA_IRQ_0]);
}

TEST_CASE_FIXTURE(WriteFixture, "late write IRQ handles all DMA blocks completed meanwhile, in order") {
    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);

    holdWriteIrq = true;                        // 41 pulses = 10 DMA blocks done before the IRQ gets handled
    captureBytes(WRITTEN);
    CHECK(wrBufferLength() == 12);              // nothing processed yet

    releaseWriteIrq();                          // one IRQ for all the blocks
    CHECK(wrBufferLength() == 12 + WRITTEN.size());

    writeEnd();
    CHECK(takeWrittenData() == WRITTEN);
}

TEST_CASE_FIXTURE(WriteFixture, "late write IRQ in the middle of a write") {
    std::vector<uint8_t> data = writtenBytes(300);

    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(std::vector<uint8_t>(data.begin(), data.begin() + 100));

    holdWriteIrq = true;
    captureBytes(std::vector<uint8_t>(data.begin() + 100, data.begin() + 110));    // 40 pulses waiting in the 64 pulses ring
    releaseWriteIrq();

    captureBytes(std::vector<uint8_t>(data.begin() + 110, data.end()));
    writeEnd();

    CHECK(takeWrittenData() == data);
}

TEST_CASE_FIXTURE(WriteFixture, "write end stores the pulses of the last, not completed DMA block") {
    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);                // 1 + 5 * 4 = 21 pulses, IRQs after 4, 8 .. 20 - the last pulse has no IRQ
    captureBytes(std::vector<uint8_t>(WRITTEN.begin(), WRITTEN.begin() + 5));

    CHECK(wrBufferLength() == 12 + 4);          // last byte not complete yet: 3 symbols processed, 1 waiting in the ring

    writeEnd();
    CHECK(takeWrittenData() == std::vector<uint8_t>(WRITTEN.begin(), WRITTEN.begin() + 5));
}

TEST_CASE_FIXTURE(WriteFixture, "write end stores the partial last byte, padded with 4 us symbols") {
    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(std::vector<uint8_t>(WRITTEN.begin(), WRITTEN.begin() + 3));
    pioPushSymbol(MFM_8US);
    pioPushSymbol(MFM_6US);

    writeEnd();

    std::vector<uint8_t> expected(WRITTEN.begin(), WRITTEN.begin() + 3);
    expected.push_back((MFM_8US << 6) | (MFM_6US << 4) | (MFM_4US << 2) | MFM_4US);
    CHECK(takeWrittenData() == expected);
}

TEST_CASE_FIXTURE(WriteFixture, "pulses that came before the write start was noticed are kept - the 00 preamble isn't lost") {
    // the WGATE IRQ can come late (e.g. while the read stream refill IRQ runs) - the write DMA IRQ already handled the
    // first pulses of the write with no wrBuffer; they're the 00 preamble, which the read back of the patched sector needs
    pioPushPulse(PIO_TICKS_GAP);                // the first pulse of the write: the interval since the previous write
    captureBytes({0x55, 0x55, 0x55});           // 12 pulses of 4 us - 3 DMA blocks, the IRQ handles them with no wrBuffer
    pioPushSymbol(MFM_4US);
    pioPushSymbol(MFM_4US);
    pioPushSymbol(MFM_4US);
    REQUIRE(wrBuffer == NULL);

    writeGateLow();
    pioPushSymbol(MFM_4US);
    captureBytes(WRITTEN);
    writeEnd();

    std::vector<uint8_t> expected = {0x55, 0x55, 0x55, 0x55};  // the gap skipped, 16 pulses of 4 us from the write start
    expected.insert(expected.end(), WRITTEN.begin(), WRITTEN.end());
    CHECK(takeWrittenData() == expected);
}

TEST_CASE_FIXTURE(WriteFixture, "write start noticed late - pulses before its gap pulse (e.g. other drive's write) aren't taken") {
    pioPushPulse(PIO_TICKS_GAP);                // some earlier pulses not belonging to this write
    captureBytes({0xe7, 0x9d});
    pioPushPulse(PIO_TICKS_GAP);                // this write starts here
    captureBytes({0x55, 0x55});
    REQUIRE(wrBuffer == NULL);

    writeGateLow();
    captureBytes(WRITTEN);
    writeEnd();

    std::vector<uint8_t> expected = {0x55, 0x55};
    expected.insert(expected.end(), WRITTEN.begin(), WRITTEN.end());
    CHECK(takeWrittenData() == expected);
}

TEST_CASE_FIXTURE(WriteFixture, "between writes only the newest pulses are kept - a long run of them doesn't overrun the ring") {
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(std::vector<uint8_t>(400, 0xe7));     // 1600 pulses with no write captured - the ring wraps many times
    pioPushPulse(PIO_TICKS_GAP);                // the write's first pulse
    captureBytes({0x55});
    REQUIRE(wrBuffer == NULL);

    writeGateLow();
    captureBytes(WRITTEN);
    writeEnd();

    std::vector<uint8_t> expected = {0x55};
    expected.insert(expected.end(), WRITTEN.begin(), WRITTEN.end());
    CHECK(takeWrittenData() == expected);
}

TEST_CASE_FIXTURE(WriteFixture, "nothing of one write leaks into the next one") {
    // write 1 ends with 2 extra symbols, so neither the ring nor the byte packing ends on a byte boundary
    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(std::vector<uint8_t>(WRITTEN.begin(), WRITTEN.begin() + 3));
    pioPushSymbol(MFM_8US);
    pioPushSymbol(MFM_8US);
    writeEnd();

    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(std::vector<uint8_t>(WRITTEN.begin() + 3, WRITTEN.end()));
    writeEnd();

    CHECK(takeWrittenData().size() == 4);
    CHECK(takeWrittenData() == std::vector<uint8_t>(WRITTEN.begin() + 3, WRITTEN.end()));
}

TEST_CASE_FIXTURE(WriteFixture, "write start never waits for a buffer - without a prepared one the write is dropped") {
    // core0 holds every buffer, e.g. host not connected for long: the writes it keeps + the two prepared ones are all
    int timeReads = 0;
    fakeOnTimeRead = [&]() {
        if (++timeReads > 1000) {
            fakeNowMs += 1000;                  // stuck waiting for the clock - let it go on, so a check fails instead of a hang
        }
    };

    while (ipcGetFreeBuffer(0, 0) != NULL) {
    }

    timeReads = 0;                              // what the main loop does to prepare a buffer: one look, no waiting
    CHECK(ipcGetFreeBuffer(0, 0) == NULL);
    CHECK(timeReads <= 2);
    fakeOnTimeRead = nullptr;

    fakeMillisStep = 1;
    for (int w = 0; w < 2; w++) {               // the two prepared buffers get used, nothing new can be prepared
        writeGateLow();
        pioPushPulse(PIO_TICKS_GAP);
        captureBytes(WRITTEN);
        writeEnd();
    }

    uint32_t start = fakeNowMs;
    writeGateLow();                             // third write - no buffer
    CHECK(fakeNowMs - start <= 1);              // didn't wait (used to wait up to 500 ms, in the main loop)
    CHECK(wrBuffer == NULL);
    CHECK(wrWritesDropped == 1);
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(WRITTEN);
    writeEnd();
    CHECK(fakeIrqsOffMaxMs <= 1);

    CHECK(takeWrittenData() == WRITTEN);        // the two captured ones, nothing for the dropped one
    CHECK(takeWrittenData() == WRITTEN);
    CHECK(ipcGetBufferFromFifo(0) == NULL);

    handleWriteBuffers();                       // takeWrittenData() freed them, as core0 does after sending - main loop prepares again
    writeGateLow();
    REQUIRE(wrBuffer != NULL);
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(WRITTEN);
    writeEnd();
    CHECK(takeWrittenData() == WRITTEN);
}

TEST_CASE_FIXTURE(WriteFixture, "write while the core1 main loop is busy is captured from its first pulse, stored once the loop runs") {
    // the main loop is busy e.g. ~14 ms loading tracks from PSRAM after a step - the write is started by the WGATE IRQ anyway
    const int track = 5, side = 0, sector = 3;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data = writtenBytes(700);
    insertTrack(track, side, t, sector);

    fakeGpioSet(PIN_WGATE, false);              // only IRQs run during this write
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(data);
    streamOn(data.size() + READ_AHEAD_MIN_BYTES);
    fakeGpioSet(PIN_WGATE, true);

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);    // not stored yet
    CHECK(ipcGetBufferFromFifo(0) == NULL);

    handleWriteBuffers();                       // main loop free again

    std::vector<uint8_t> expected = expectedAfterWrite(t, sector, data);
    CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == expected);
    CHECK(takeWrittenData() == data);           // whole, from the first byte
}

TEST_CASE_FIXTURE(WriteFixture, "two writes finished while the main loop is busy are both stored, in order") {
    const int track = 7, side = 0;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    std::vector<uint8_t> data3 = writtenBytes(700);
    std::vector<uint8_t> data4 = writtenBytes(680, 101);

    insertTrack(track, side, t, 3);
    fakeGpioSet(PIN_WGATE, false);
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(data3);
    fakeGpioSet(PIN_WGATE, true);

    streamToSector(4);
    fakeGpioSet(PIN_WGATE, false);
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(data4);
    fakeGpioSet(PIN_WGATE, true);

    handleWriteBuffers();

    std::vector<uint8_t> expected = expectedAfterWrite(expectedAfterWrite(t, 3, data3), 4, data4);
    CHECK(memcmp(trackData0, expected.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(takeWrittenData() == data3);
    CHECK(takeWrittenData() == data4);
}

TEST_CASE_FIXTURE(WriteFixture, "both WGATE edges in one IRQ - order taken from the current level") {
    SUBCASE("WGATE high now: a write started and ended - nothing captured, nothing stored") {
        int freeBefore = freeCore0Buffers();
        fakeGpioIrq(PIN_WGATE, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
        CHECK(wrBuffer == NULL);
        handleWriteBuffers();
        CHECK(ipcGetBufferFromFifo(0) == NULL);
        CHECK(freeCore0Buffers() == freeBefore);
    }

    SUBCASE("WGATE low now: the write ended and a new one started") {
        std::vector<uint8_t> first(WRITTEN.begin(), WRITTEN.begin() + 5);
        std::vector<uint8_t> second(WRITTEN.begin() + 5, WRITTEN.end());

        writeGateLow();
        pioPushPulse(PIO_TICKS_GAP);
        captureBytes(first);

        fakeGpioIrq(PIN_WGATE, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);     // WGATE stays low
        REQUIRE(wrBuffer != NULL);
        pioPushPulse(PIO_TICKS_GAP);
        captureBytes(second);
        writeEnd();

        CHECK(takeWrittenData() == first);
        CHECK(takeWrittenData() == second);
    }
}

}

TEST_SUITE("fdd empty write") {

TEST_CASE_FIXTURE(WriteFixture, "WGATE toggled without any data leaves the sector untouched and sends nothing") {
    const int track = 5, side = 0, sector = 3;
    std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
    insertTrack(track, side, t, sector);
    int freeBefore = freeCore0Buffers();

    SUBCASE("no pulse at all") {
        writeGateLow();
        writeEnd();
    }

    SUBCASE("only the first interval, rejected as too long") {
        writeGateLow();
        pioPushPulse(PIO_TICKS_GAP);
        writeEnd();
    }

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);    // used to clear the whole data part of the sector
    CHECK(psramTrack(track, side) == t);
    CHECK(ipcGetBufferFromFifo(0) == NULL);                                // host ignores empty writes, nothing to send
    CHECK(freeCore0Buffers() == freeBefore);                               // buffer given back (and a new one prepared)
}

}

TEST_SUITE("fdd write to sector not in track buffer") {

// The written sector is searched for in the track buffer by its header. When it isn't there, the search used to
// loop forever on the next tag it didn't expect - hanging core1 (no INDEX, no track restart, no stepping).

TEST_CASE_FIXTURE(WriteFixture, "step after the write - track buffer already holds the next track when the main loop stores the write") {
    const int side = 0, sector = 3;
    std::vector<uint8_t> t5 = buildEncodedTrack(5, side, 9);
    std::vector<uint8_t> t6 = buildEncodedTrack(6, side, 9);
    std::vector<uint8_t> data = writtenBytes(700);

    psramStoreTrack(6, side, t6.data());
    insertTrack(5, side, t5, sector);

    writeGateLow();
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(data);
    streamOn(data.size() + READ_AHEAD_MIN_BYTES);
    fakeGpioSet(PIN_WGATE, true);               // write ended, main loop didn't store it yet...

    memcpy(trackData0, t6.data(), READTRACKDATA_SIZE_BYTES);   // ...the ST stepped, STATE_LOADING loaded track 6 first
    handleWriteBuffers();                       // must return

    CHECK(memcmp(trackData0, t6.data(), READTRACKDATA_SIZE_BYTES) == 0);   // track 6 not damaged
    CHECK(psramTrack(6, side) == t6);
    CHECK(takeWrittenData() == data);           // host still gets the write
}

TEST_CASE("sector header without data part - search stops, nothing stored") {
    const int track = 5, side = 0;
    std::vector<uint8_t> data = writtenBytes(700);
    ipcInit();
    int sector = 0;

    SUBCASE("next sector header comes first")   { sector = 3; }
    SUBCASE("track stream end comes first")     { sector = 9; }

    std::vector<uint8_t> t = buildTestTrack(track, side, 9, sector);
    memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);

    storeWrittenSectorDataToTrackLocally(side, track, sector, data.data(), data.size());
    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
}

}

TEST_SUITE("fdd written sector decoding") {

namespace {

struct DecodeFixture {
    const int track = 5, side = 0, sector = 3;
    std::vector<uint8_t> t = buildTestTrack(5, 0, 9);
    std::vector<uint8_t> data = testSectorData(77);

    DecodeFixture() {
        fakeReset();
        ipcInit();
        memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);
        psramStoreTrack(track, side, trackData0);
    }

    void write(const std::vector<uint8_t>& symbols) {
        storeWrittenSectorDataToTrackLocally(side, track, sector, (uint8_t*) symbols.data(), symbols.size());
    }

    std::vector<uint8_t> trackNow(void) {
        return std::vector<uint8_t>(trackData0, trackData0 + READTRACKDATA_SIZE_BYTES);
    }

    void checkUnchanged(void) {
        CHECK(trackNow() == t);
        CHECK(psramTrack(track, side) == t);
    }

    void checkStored(const std::vector<uint8_t>& run) {
        std::vector<uint8_t> expected = t;
        std::copy(run.begin(), run.end(), expected.begin() + testFindRun(t, sector));
        CHECK(trackNow() == expected);
        CHECK(psramTrack(track, side) == expected);
    }
};

}   // namespace

TEST_CASE_FIXTURE(DecodeFixture, "written sector is stored into its raw run, with nothing else in the track moved") {
    std::vector<uint8_t> run = testSectorRun(data);
    write(testWriteSymbols(run));
    checkStored(run);
}

TEST_CASE_FIXTURE(DecodeFixture, "every byte value decodes back, F5 and the mark values too") {
    for(int i=0; i<512; i++) {
        data[i] = i;
    }
    std::vector<uint8_t> run = testSectorRun(data);
    write(testWriteSymbols(run));
    checkStored(run);
}

TEST_CASE_FIXTURE(DecodeFixture, "CRC off by 1 is accepted, the stored CRC is the right one so the sector reads back fine") {
    std::vector<uint8_t> good = testSectorRun(data);
    std::vector<uint8_t> received = good;
    uint16_t crc = (((uint16_t) received[513] << 8) | received[514]) + 1;
    received[513] = crc >> 8;
    received[514] = crc & 0xff;

    write(testWriteSymbols(received));
    checkStored(good);
}

TEST_CASE_FIXTURE(DecodeFixture, "end of the CRC not captured, the whole data was - accepted, CRC calculated") {
    std::vector<uint8_t> run = testSectorRun(data);
    std::vector<uint8_t> symbols = testWriteSymbols(std::vector<uint8_t>(run.begin(), run.end() - 2), 0);
    write(symbols);
    checkStored(run);
}

TEST_CASE_FIXTURE(DecodeFixture, "data not captured whole - the old sector stays") {
    std::vector<uint8_t> run = testSectorRun(data);
    write(testWriteSymbols(std::vector<uint8_t>(run.begin(), run.begin() + 300), 0));
    checkUnchanged();
}

TEST_CASE_FIXTURE(DecodeFixture, "damaged data - CRC doesn't match, the old sector stays") {
    std::vector<uint8_t> run = testSectorRun(data);
    run[200] ^= 0x10;
    write(testWriteSymbols(run));
    checkUnchanged();
}

TEST_CASE_FIXTURE(DecodeFixture, "no sync in the write, or another mark than DAM - the old sector stays") {
    SUBCASE("no sync") {
        write(std::vector<uint8_t>(800, 0x55));
    }

    SUBCASE("deleted data mark") {
        std::vector<uint8_t> run = testSectorRun(data);
        run[0] = 0xf8;
        write(testWriteSymbols(run));
    }

    SUBCASE("nothing") {
        write(std::vector<uint8_t>());
    }

    checkUnchanged();
}

TEST_CASE_FIXTURE(DecodeFixture, "sector 0 or beyond the limit doesn't exist") {
    std::vector<uint8_t> symbols = testWriteSymbols(testSectorRun(data));

    storeWrittenSectorDataToTrackLocally(side, track, 0, symbols.data(), symbols.size());
    storeWrittenSectorDataToTrackLocally(side, track, MAX_SECTORS_PER_TRACK + 1, symbols.data(), symbols.size());
    checkUnchanged();
}

TEST_CASE_FIXTURE(DecodeFixture, "FORMAT TRACK clears all the sectors of the track, as the host does in the image") {
    std::vector<uint8_t> formatWrite = testWriteSymbols({0xfe, (uint8_t) track, (uint8_t) side, 1, 2, 0x12, 0x34});
    write(formatWrite);

    std::vector<uint8_t> empty = testSectorRun(std::vector<uint8_t>(512, 0));

    for(int s=1; s<=9; s++) {
        CAPTURE(s);
        std::vector<uint8_t> now = trackNow();
        int offset = testFindRun(now, s);
        REQUIRE(offset > 0);
        CHECK(std::vector<uint8_t>(now.begin() + offset, now.begin() + offset + MFM_SECTOR_RUN_SIZE) == empty);
    }

    CHECK(psramTrack(track, side) == trackNow());
}

TEST_CASE_FIXTURE(DecodeFixture, "FORMAT TRACK when another track is in the buffer already, after a step - nothing changed") {
    std::vector<uint8_t> formatWrite = testWriteSymbols({0xfe, 4, (uint8_t) side, 1, 2, 0x12, 0x34});
    storeWrittenSectorDataToTrackLocally(side, 4, 1, formatWrite.data(), formatWrite.size());
    checkUnchanged();
}

TEST_CASE("written data read back: raw run after the write gives the intervals of the write") {
    // the whole way: ST writes symbols -> stored as a raw run -> the stream makes the same intervals
    fakeReset();
    ipcInit();
    mfmStreamGlobalInit(150000000);

    std::vector<uint8_t> t = buildTestTrack(5, 0, 9);
    memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);

    std::vector<uint8_t> run = testSectorRun(testSectorData(55));
    std::vector<uint8_t> symbols = testWriteSymbols(run);
    storeWrittenSectorDataToTrackLocally(0, 5, 3, symbols.data(), symbols.size());

    SymbolEncoder written;
    written.bytes(std::vector<uint8_t>(12, 0));
    for(int i=0; i<3; i++) {
        written.syncA1();
    }
    written.bytes(run);
    std::vector<int> expected(written.intervals.end() - 2500, written.intervals.end() - 8);    // the data part of the sector

    MfmStream s = MfmStream();
    mfmStreamStart(&s, trackData0, READTRACKDATA_SIZE_BYTES);

    std::vector<int> all;
    uint16_t out[1024];
    while(!s.ended) {
        mfmStreamFill(&s, out, 1024);
        for(uint16_t top : out) {
            all.push_back((top + 1) / 150);
        }
    }

    CHECK(std::search(all.begin(), all.end(), expected.begin(), expected.end()) != all.end());
}

}
