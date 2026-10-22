// core1_main.cpp + mfm_write.cpp - written sector capture: the bytes captured between write start and write end must be
// stored locally (track buffer in RAM + PSRAM) into the sector the ST was writing, and sent to core0 for the host.
//
// The track buffers hold the encoded track format of ce_pico_fdd/encoded_track_format.txt (as produced by the CE server).
// The ST writes a sector right after reading its header, so the write belongs to the sector whose CMD_CURRENT_SECTOR mark
// the stream passed last when the write started. The stream is read ahead into the DMA ring, so by the time the write
// ends, posStreamed already holds the next sector - the tests reproduce that by streaming on between write start and end.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "psram.h"
#include "mfm_write.h"
#include "core1_main.h"

// firmware globals / functions not in the firmware headers
extern TDrivePosition posStreamed;
extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];
extern uint8_t imgSectorsPerTrack;

extern uint32_t dataIndexInTrack;                       // core1_main.cpp - read index in the streamed track
extern volatile IPCbuffer* wrBuffer;                    // core1_main.cpp - buffer the write IRQ appends to
void getMfmDataToBuffer(uint8_t* bfr, int len);         // core1_main.cpp - takes the next MFM bytes of the track, updates posStreamed

extern volatile uint8_t wrStreamByte;                   // mfm_write.cpp - byte packing of the captured symbols
extern volatile uint8_t wrBits;

extern IPCbuffer buffersForCore0[IPC_BUFFER_SMALL_COUNT];  // ipc.cpp - write buffers going to core0
extern volatile uint32_t wrWritesDropped;               // core1_main.cpp - writes not captured

namespace {

const int TRACK_STREAM_TABLE_BYTES = 2 * STREAM_TABLE_ITEMS;   // stream table as the server makes it - 20 words
const int TRACK_START_GAP_BYTES    = 60;
const int SECTOR_HEADER_MFM_BYTES  = 40;    // MFM bytes between the CMD_CURRENT_SECTOR mark and CMD_DATA_PART_OF_SECTOR
const int SECTOR_DATA_MFM_BYTES    = 760;   // MFM bytes after CMD_DATA_PART_OF_SECTOR (12x 00, 3x A1, DAM, 512 B, CRC, 40x 4E), then zero padding
const int READ_AHEAD_MIN_BYTES     = 256;   // stream read ahead of the output: at least one DMA ring half (MFM_READ_SIZE_FILLS)
const int SECTOR_DATA_OFFSET       = 4 + SECTOR_HEADER_MFM_BYTES + 1;  // from sector start to first byte of data part
const int SECTOR_DATA_SPACE        = ENCODED_SECTOR_MAX_SIZE - 1 - SECTOR_DATA_OFFSET;  // data bytes the device may overwrite

int sectorOffset(int sector)        // 1-based sector number -> offset of its CMD_CURRENT_SECTOR mark in the track buffer
{
    return TRACK_STREAM_TABLE_BYTES + TRACK_START_GAP_BYTES + (sector - 1) * ENCODED_SECTOR_MAX_SIZE;
}

// valid MFM byte (four 2-bit symbols, none 00), so it never looks like a stream tag or padding - different per sector
uint8_t sectorPattern(int sector)
{
    static const uint8_t patterns[] = { 0x55, 0x56, 0x57, 0x59, 0x5a, 0x5b, 0x5d, 0x5e, 0x5f, 0x65, 0x66, 0x67 };
    return patterns[sector % 12];
}

// encoded track like the server makes it, see ce_pico_fdd/encoded_track_format.txt
std::vector<uint8_t> buildEncodedTrack(int track, int side, int spt)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES, 0);

    for (int i = 0; i < TRACK_START_GAP_BYTES; i++) {
        t[TRACK_STREAM_TABLE_BYTES + i] = 0x55;
    }

    for (int s = 1; s <= spt; s++) {
        int o = sectorOffset(s);
        storeWord(&t[2 * s], o);            // stream table: offset of sector s

        t[o + 0] = CMD_CURRENT_SECTOR;
        t[o + 1] = side;
        t[o + 2] = track;
        t[o + 3] = s;

        for (int i = 0; i < SECTOR_HEADER_MFM_BYTES; i++) {
            t[o + 4 + i] = 0x55;
        }

        t[o + 4 + SECTOR_HEADER_MFM_BYTES] = CMD_DATA_PART_OF_SECTOR;

        for (int i = 0; i < SECTOR_DATA_MFM_BYTES; i++) {
            t[o + SECTOR_DATA_OFFSET + i] = sectorPattern(s);
        }
    }

    int end = sectorOffset(spt + 1);
    t[end] = CMD_TRACK_STREAM_END;
    storeWord(&t[0], end + 2);              // stream table: end of stream

    return t;
}

// the bytes the ST writes - valid MFM bytes, distinct from all sector patterns
std::vector<uint8_t> writtenBytes(int count)
{
    static const uint8_t vals[] = { 0x6b, 0xe7, 0x9d, 0xb6 };
    std::vector<uint8_t> v(count);
    for (int i = 0; i < count; i++) {
        v[i] = vals[i % 4];
    }
    return v;
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

// read the stream on, as the DMA refills do while the ST is writing - this moves posStreamed
void streamOn(int bytes)
{
    std::vector<uint8_t> out(bytes);
    getMfmDataToBuffer(out.data(), bytes);
}

std::vector<uint8_t> psramTrack(int track, int side)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES);
    psramLoadTrack(track, side, t.data());
    return t;
}

// what the track must look like after 'data' was written into 'sector' - written data, rest of the data part cleared
std::vector<uint8_t> expectedAfterWrite(std::vector<uint8_t> t, int sector, const std::vector<uint8_t>& data)
{
    int start = sectorOffset(sector) + SECTOR_DATA_OFFSET;

    for (int i = 0; i < SECTOR_DATA_SPACE; i++) {
        t[start + i] = (i < (int) data.size()) ? data[i] : 0;
    }

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

        dataIndexInTrack = sectorOffset(sector);
        streamOn(SECTOR_HEADER_MFM_BYTES);              // the ST reads the sector header...
        REQUIRE(posStreamed.sector == sector);
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
    // the premise of the tests below: a 512 B sector write is about 700 MFM bytes long, with the read-ahead the stream
    // passes the end of the data part (about 760 MFM bytes) and the next sector header before WGATE goes high
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
        CHECK(trackData0[sectorOffset(sector + 1) + SECTOR_DATA_OFFSET] == sectorPattern(sector + 1));  // next sector untouched
        CHECK(trackData0[sectorOffset(sector) + SECTOR_DATA_OFFSET] == data[0]);
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
    const int track = 80, side = 1, sector = 2;        // track 80 == 0x50 == CMD_CURRENT_SECTOR, must not confuse the parsing
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
    std::vector<uint8_t> data4(680, 0xb6);

    insertTrack(track, side, t, 3);
    writeSector(data3);
    REQUIRE(posStreamed.sector == 4);

    dataIndexInTrack = sectorOffset(4);                 // the ST reads sector 4 header now
    streamOn(SECTOR_HEADER_MFM_BYTES);
    REQUIRE(posStreamed.sector == 4);

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
    dataIndexInTrack = sectorOffset(4);
    streamOn(SECTOR_HEADER_MFM_BYTES);
    REQUIRE(posStreamed.sector == 4);
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

TEST_CASE_FIXTURE(WriteFixture, "pulses handled before the write start was noticed don't shift the byte packing of the write") {
    // core1 polls WGATE, so the first pulses of a write can be processed by the IRQ while wrBuffer is still NULL - they are
    // lost (they're the 00 preamble), but the bytes after them must stay aligned
    pioPushPulse(PIO_TICKS_GAP);                // 4 pulses = one DMA block, the IRQ handles it with no wrBuffer
    pioPushSymbol(MFM_4US);
    pioPushSymbol(MFM_4US);
    pioPushSymbol(MFM_4US);
    REQUIRE(wrBuffer == NULL);

    writeGateLow();
    captureBytes(WRITTEN);
    writeEnd();

    CHECK(takeWrittenData() == WRITTEN);
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
    std::vector<uint8_t> data4(680, 0xb6);

    insertTrack(track, side, t, 3);
    fakeGpioSet(PIN_WGATE, false);
    pioPushPulse(PIO_TICKS_GAP);
    captureBytes(data3);
    fakeGpioSet(PIN_WGATE, true);

    dataIndexInTrack = sectorOffset(4);
    streamOn(SECTOR_HEADER_MFM_BYTES);
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

    SUBCASE("next sector header comes first") {
        std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
        t[sectorOffset(3) + 4 + SECTOR_HEADER_MFM_BYTES] = 0x55;    // sector 3 has no CMD_DATA_PART_OF_SECTOR
        memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);

        storeWrittenSectorDataToTrackLocally(side, track, 3, data.data(), data.size());
        CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
    }

    SUBCASE("track stream end comes first") {
        std::vector<uint8_t> t = buildEncodedTrack(track, side, 9);
        t[sectorOffset(9) + 4 + SECTOR_HEADER_MFM_BYTES] = 0x55;    // last sector has no CMD_DATA_PART_OF_SECTOR
        memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);

        storeWrittenSectorDataToTrackLocally(side, track, 9, data.data(), data.size());
        CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
    }
}

}

TEST_SUITE("fdd write with bad sector offset") {

// The sector offset from the stream table must leave room for the whole sector (ENCODED_SECTOR_MAX_SIZE) in the track
// buffer and must point beyond the stream table - otherwise the write is not stored locally at all.

struct BadOffsetFixture {
    const int track = 5, side = 0;
    std::vector<uint8_t> t = buildEncodedTrack(5, 0, 9);
    std::vector<uint8_t> data = writtenBytes(700);

    BadOffsetFixture() {
        fakeReset();
        ipcInit();
    }

    // point sector 3 to 'offset', put a sector 3 header + data part mark there, then write sector 3
    void writeWithOffset(int offset) {
        storeWord(&t[2 * 3], offset);

        if (offset + 4 + SECTOR_HEADER_MFM_BYTES < READTRACKDATA_SIZE_BYTES) {
            t[offset + 0] = CMD_CURRENT_SECTOR;
            t[offset + 1] = side;
            t[offset + 2] = track;
            t[offset + 3] = 3;
            t[offset + 4 + SECTOR_HEADER_MFM_BYTES] = CMD_DATA_PART_OF_SECTOR;
        }

        memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);
        psramStoreTrack(track, side, trackData0);

        storeWrittenSectorDataToTrackLocally(side, track, 3, data.data(), data.size());
    }
};

TEST_CASE_FIXTURE(BadOffsetFixture, "sector in the last ENCODED_SECTOR_MAX_SIZE bytes of the track - would write past the track buffer") {
    writeWithOffset(READTRACKDATA_SIZE_BYTES - ENCODED_SECTOR_MAX_SIZE + 1);

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == t);
}

TEST_CASE_FIXTURE(BadOffsetFixture, "sector exactly at the last possible offset is stored, up to the track buffer end") {
    const int offset = READTRACKDATA_SIZE_BYTES - ENCODED_SECTOR_MAX_SIZE;
    writeWithOffset(offset);

    int start = offset + SECTOR_DATA_OFFSET;
    CHECK(trackData0[start] == data[0]);
    CHECK(trackData0[start + data.size() - 1] == data.back());
    CHECK(trackData0[READTRACKDATA_SIZE_BYTES - 1] == 0);
    CHECK(psramTrack(track, side)[start] == data[0]);
}

TEST_CASE_FIXTURE(BadOffsetFixture, "sector offset 0 (sector not in track) or inside the stream table") {
    int offset = 0;
    SUBCASE("offset 0")              { offset = 0; }
    SUBCASE("inside stream table")   { offset = 2 * STREAM_TABLE_ITEMS - 1; }

    writeWithOffset(offset);

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
    CHECK(psramTrack(track, side) == t);
}

TEST_CASE_FIXTURE(BadOffsetFixture, "sector 0 doesn't exist - its stream table item is the end of stream offset") {
    memcpy(trackData0, t.data(), READTRACKDATA_SIZE_BYTES);
    storeWrittenSectorDataToTrackLocally(side, track, 0, data.data(), data.size());

    CHECK(memcmp(trackData0, t.data(), READTRACKDATA_SIZE_BYTES) == 0);
}

}
