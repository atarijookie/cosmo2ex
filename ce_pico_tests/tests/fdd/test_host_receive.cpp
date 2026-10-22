// host_receive.cpp - messages from host: header, then 'len' bytes of data. Every message must be read whole, so the next
// header is found right after it - a track must reach core1 only when it was received completely, with no stale bytes of
// an earlier track, and a message that can't be handled now (no free buffer) must wait, not be lost.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_connection.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "host_receive.h"

extern IPCbuffer buffersForCore1[IPC_BUFFER_LARGE_COUNT];  // ipc.cpp
extern int imageState;
extern char imageFileName[32];
extern uint8_t imgTracks, imgSides, imgSectorsPerTrack;

namespace {

std::vector<uint8_t> message(uint16_t cmd, const std::vector<uint8_t>& data, uint32_t len)
{
    std::vector<uint8_t> m(TX_HEADER_SIZE);
    storeHeader(m.data(), cmd, len);
    m.insert(m.end(), data.begin(), data.end());
    return m;
}

// track message as the host sends it: track #, side #, then the encoded stream
std::vector<uint8_t> trackData(int track, int side, int streamLength, uint8_t fill)
{
    std::vector<uint8_t> d(2 + streamLength, fill);
    d[0] = track;
    d[1] = side;
    return d;
}

std::vector<uint8_t> trackMessage(int track, int side, int streamLength, uint8_t fill)
{
    std::vector<uint8_t> d = trackData(track, side, streamLength, fill);
    return message(ATN_SEND_TRACK, d, d.size());
}

// image start / end message: 1 = end / 0 = start, tracks, sides, sectors per track, file name
std::vector<uint8_t> imageData(bool end, const char* name)
{
    std::vector<uint8_t> d = { (uint8_t) (end ? 1 : 0), 80, 2, 9 };
    d.insert(d.end(), name, name + strlen(name) + 1);
    return d;
}

int freeCore1Buffers(void)
{
    int n = 0;
    for (int i = 0; i < IPC_BUFFER_LARGE_COUNT; i++) {
        n += buffersForCore1[i].free ? 1 : 0;
    }
    return n;
}

// the track core1 got next: track #, side #, the whole received data
struct ReceivedTrack {
    int track, side;
    std::vector<uint8_t> data;
};

ReceivedTrack takeTrackForCore1(void)
{
    IPCbuffer* bfr = ipcGetBufferFromFifo(1);
    REQUIRE(bfr != NULL);

    IpcCommand command = bfr->command;      // IPCbuffer is packed - copy fields before CHECK binds them
    CHECK(command == CMD_STORE_TRACK);

    ReceivedTrack t = { bfr->trackNo, bfr->sideNo, std::vector<uint8_t>(bfr->data, bfr->data + bfr->length) };
    bfr->free = true;
    return t;
}

struct HostReceiveFixture {
    HostReceiveFixture() {
        fakeReset();
        fakeHostReset();
        ipcInit();

        fakeHostConnected = false;          // forget a message pending from the previous test
        handleIncommingData();
        fakeHostConnected = true;

        imageState = IMAGE_NOT_LOADED;
        memset(imageFileName, 0, sizeof(imageFileName));
    }

    // what the core0 main loop does: handle incoming data on every run
    void mainLoop(int runs = 1) {
        for (int i = 0; i < runs; i++) {
            handleIncommingData();
        }
    }
};

}   // namespace

TEST_SUITE("fdd host receive") {

TEST_CASE_FIXTURE(HostReceiveFixture, "two whole tracks in a row both reach core1") {
    std::vector<uint8_t> first = trackData(5, 0, 6000, 0x55);
    std::vector<uint8_t> second = trackData(6, 1, 7000, 0x5a);
    fakeHostReceive(message(ATN_SEND_TRACK, first, first.size()));
    fakeHostReceive(message(ATN_SEND_TRACK, second, second.size()));

    mainLoop(2);

    ReceivedTrack t = takeTrackForCore1();
    CHECK(t.track == 5);
    CHECK(t.side == 0);
    CHECK(t.data == first);

    t = takeTrackForCore1();
    CHECK(t.track == 6);
    CHECK(t.side == 1);
    CHECK(t.data == second);

    CHECK(fakeHostRx.empty());
    CHECK(fakeHostDisconnects == 0);
}

TEST_CASE_FIXTURE(HostReceiveFixture, "track arriving in pieces is read until complete") {
    // the TCP window is smaller than a track - the rest comes while the first part is read
    std::vector<uint8_t> msg = trackMessage(5, 0, READTRACKDATA_SIZE_BYTES - 2, 0x55);
    size_t sent = 0;
    auto sendSome = [&]() {
        size_t n = std::min((size_t) 1460, msg.size() - sent);
        fakeHostReceive(std::vector<uint8_t>(msg.begin() + sent, msg.begin() + sent + n));
        sent += n;
    };

    sendSome();
    fakeOnTimeRead = [&]() { if (sent < msg.size()) sendSome(); };

    mainLoop();

    ReceivedTrack t = takeTrackForCore1();
    CHECK(t.data == std::vector<uint8_t>(msg.begin() + TX_HEADER_SIZE, msg.end()));
    CHECK(fakeHostDisconnects == 0);
}

TEST_CASE_FIXTURE(HostReceiveFixture, "no free buffer - track waits in the connection without blocking, not lost") {
    while (ipcGetFreeBuffer(1, 0) != NULL) {       // core1 busy storing tracks - has all buffers
    }

    std::vector<uint8_t> d = trackData(7, 0, 6000, 0x55);
    std::vector<uint8_t> msg = message(ATN_SEND_TRACK, d, d.size());
    fakeHostReceive(msg);

    uint32_t start = fakeNowMs;
    fakeMillisStep = 1;
    mainLoop(3);

    CHECK(fakeNowMs - start <= 10);                 // used to wait 500 ms for a buffer on every try
    CHECK(fakeHostRx.size() == msg.size() - TX_HEADER_SIZE);   // only the header was read, the data waits
    CHECK(ipcGetBufferFromFifo(1) == NULL);

    buffersForCore1[3].free = true;                 // core1 stored one
    mainLoop();

    ReceivedTrack t = takeTrackForCore1();
    CHECK(t.track == 7);
    CHECK(t.data == d);
    CHECK(fakeHostRx.empty());
}

TEST_CASE_FIXTURE(HostReceiveFixture, "track not received completely - nothing stored, buffer freed, connection closed") {
    std::vector<uint8_t> msg = trackMessage(5, 0, 10000, 0x55);
    msg.resize(TX_HEADER_SIZE + 4000);              // the rest never comes
    fakeHostReceive(msg);
    int freeBefore = freeCore1Buffers();

    mainLoop();

    CHECK(ipcGetBufferFromFifo(1) == NULL);         // used to store the part + stale bytes of an earlier track
    CHECK(freeCore1Buffers() == freeBefore);
    CHECK(fakeHostDisconnects == 1);                // stream position lost - reconnect requests the image again
}

TEST_CASE_FIXTURE(HostReceiveFixture, "connection lost while reading a track - nothing stored") {
    std::vector<uint8_t> msg = trackMessage(5, 0, 10000, 0x55);
    msg.resize(TX_HEADER_SIZE + 4000);
    fakeHostReceive(msg);

    int timeReads = 0;
    fakeOnTimeRead = [&]() { if (++timeReads == 5) fakeHostConnected = false; };
    uint32_t start = fakeNowMs;

    mainLoop();

    CHECK(ipcGetBufferFromFifo(1) == NULL);
    CHECK(fakeNowMs - start < MESSAGE_DATA_TIMEOUT_MS);     // didn't wait for the timeout
}

TEST_CASE_FIXTURE(HostReceiveFixture, "track longer than the buffer - the rest is dropped, the next message is found right after it") {
    std::vector<uint8_t> big = trackData(5, 0, READTRACKDATA_SIZE_BYTES + 100, 0x55);
    std::vector<uint8_t> next = trackData(6, 0, 3000, 0x5a);
    fakeHostReceive(message(ATN_SEND_TRACK, big, big.size()));
    fakeHostReceive(message(ATN_SEND_TRACK, next, next.size()));

    mainLoop(2);

    ReceivedTrack t = takeTrackForCore1();
    CHECK(t.track == 5);
    CHECK(t.data == std::vector<uint8_t>(big.begin(), big.begin() + READTRACKDATA_SIZE_BYTES));

    t = takeTrackForCore1();
    CHECK(t.track == 6);
    CHECK(t.data == next);
    CHECK(fakeHostRx.empty());
}

TEST_CASE_FIXTURE(HostReceiveFixture, "track message without track # and side # is dropped") {
    fakeHostReceive(message(ATN_SEND_TRACK, { 5 }, 1));
    int freeBefore = freeCore1Buffers();

    mainLoop();

    CHECK(ipcGetBufferFromFifo(1) == NULL);
    CHECK(freeCore1Buffers() == freeBefore);
    CHECK(fakeHostRx.empty());
}

TEST_CASE_FIXTURE(HostReceiveFixture, "track with a bad track or side number is dropped, not stored over another track") {
    int track = 0, side = 0;
    SUBCASE("track beyond the last one")    { track = MAX_TRACKS + 5; side = 0; }
    SUBCASE("track just after the last one") { track = MAX_TRACKS; side = 1; }
    SUBCASE("side 2")                       { track = 10; side = 2; }

    std::vector<uint8_t> next = trackData(6, 0, 3000, 0x5a);
    fakeHostReceive(trackMessage(track, side, 5000, 0x55));
    fakeHostReceive(message(ATN_SEND_TRACK, next, next.size()));
    int freeBefore = freeCore1Buffers();

    mainLoop(2);

    ReceivedTrack t = takeTrackForCore1();      // only the good one - the bad one used to become track 89 / side 1
    CHECK(t.track == 6);
    CHECK(ipcGetBufferFromFifo(1) == NULL);
    CHECK(freeCore1Buffers() == freeBefore);
    CHECK(fakeHostRx.empty());
}

TEST_CASE_FIXTURE(HostReceiveFixture, "image start and end") {
    fakeHostReceive(message(ATN_SEND_WHOLE_IMAGE, imageData(false, "GAME.ST"), imageData(false, "GAME.ST").size()));
    mainLoop();
    CHECK(imageState == IMAGE_REQUESTED);
    CHECK(std::string(imageFileName) == "GAME.ST");

    fakeHostReceive(message(ATN_SEND_WHOLE_IMAGE, imageData(true, "GAME.ST"), imageData(true, "GAME.ST").size()));
    mainLoop();
    CHECK(imageState == IMAGE_LOADED);
    CHECK(imgTracks == 80);
    CHECK(imgSectorsPerTrack == 9);
}

TEST_CASE_FIXTURE(HostReceiveFixture, "image message longer than 128 bytes - the rest is dropped, the next message is found") {
    std::vector<uint8_t> d = imageData(true, "GAME.ST");
    d.resize(300, 0x50);                            // 0x50 == CMD_CURRENT_SECTOR, whatever - must not be parsed
    std::vector<uint8_t> next = trackData(6, 0, 3000, 0x5a);
    fakeHostReceive(message(ATN_SEND_WHOLE_IMAGE, d, d.size()));
    fakeHostReceive(message(ATN_SEND_TRACK, next, next.size()));

    mainLoop(2);

    CHECK(imageState == IMAGE_LOADED);
    CHECK(takeTrackForCore1().data == next);
}

TEST_CASE_FIXTURE(HostReceiveFixture, "image message not received completely - not used") {
    std::vector<uint8_t> msg = message(ATN_SEND_WHOLE_IMAGE, imageData(true, "GAME.ST"), 40);
    msg.resize(TX_HEADER_SIZE + 2);                 // only 2 of 40 bytes come
    fakeHostReceive(msg);
    imageState = IMAGE_REQUESTED;

    mainLoop();

    CHECK(imageState == IMAGE_REQUESTED);           // used to act on uninitialised stack bytes
    CHECK(fakeHostDisconnects == 1);
}

TEST_CASE_FIXTURE(HostReceiveFixture, "short image message - missing bytes are zeros, not stale stack content") {
    fakeHostReceive(message(ATN_SEND_WHOLE_IMAGE, { 0, 80, 2, 9 }, 4));      // no file name
    strcpy(imageFileName, "OLD.ST");

    mainLoop();

    CHECK(imageState == IMAGE_REQUESTED);
    CHECK(std::string(imageFileName) == "");
}

TEST_CASE_FIXTURE(HostReceiveFixture, "unknown message - its data is skipped, the next message is found") {
    std::vector<uint8_t> next = trackData(6, 0, 3000, 0x5a);
    fakeHostReceive(message(0x77, std::vector<uint8_t>(500, 0xc0), 500));
    fakeHostReceive(message(ATN_SEND_TRACK, next, next.size()));

    mainLoop(2);

    CHECK(takeTrackForCore1().data == next);
    CHECK(fakeHostRx.empty());
}

TEST_CASE_FIXTURE(HostReceiveFixture, "message waiting for a buffer is forgotten when the connection is lost") {
    while (ipcGetFreeBuffer(1, 0) != NULL) {
    }

    fakeHostReceive(trackMessage(7, 0, 6000, 0x55));
    mainLoop();                                     // header read, waits for a buffer

    fakeHostConnected = false;                      // connection lost - its data is gone with it
    fakeHostRx.clear();
    mainLoop();

    buffersForCore1[0].free = true;
    fakeHostConnected = true;                       // new connection
    std::vector<uint8_t> d = trackData(8, 1, 3000, 0x5a);
    fakeHostReceive(message(ATN_SEND_TRACK, d, d.size()));
    mainLoop();

    ReceivedTrack t = takeTrackForCore1();
    CHECK(t.track == 8);                            // not read as the data of the old header
    CHECK(t.data == d);
}

}
