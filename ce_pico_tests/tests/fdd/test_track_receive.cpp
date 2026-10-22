// core1_main.cpp - storeReceivedTrack(): a track received from the host (track #, side #, then the encoded stream)
// must end up in PSRAM / the track buffer as exactly that stream followed by zeros - nothing of an earlier track that
// went through the same IPC buffer, and nothing read from beyond the received data.

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "ipc.h"
#include "psram.h"
#include "core1_main.h"

extern TDrivePosition hwPosition;
extern uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
extern uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

namespace {

// encoded stream of given length: MFM bytes, then CMD_TRACK_STREAM_END + 0 as the server ends it
std::vector<uint8_t> makeStream(int length, uint8_t fill)
{
    std::vector<uint8_t> s(length, fill);
    s[length - 2] = CMD_TRACK_STREAM_END;
    s[length - 1] = 0;
    return s;
}

// what the whole track must look like: the stream, then zeros
std::vector<uint8_t> wholeTrack(const std::vector<uint8_t>& stream)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES, 0);
    std::copy(stream.begin(), stream.end(), t.begin());
    return t;
}

std::vector<uint8_t> psramTrack(int track, int side)
{
    std::vector<uint8_t> t(READTRACKDATA_SIZE_BYTES);
    psramLoadTrack(track, side, t.data());
    return t;
}

// fill the buffer like handleTrackReceived() does with the received message, then let core1 store it
void receiveTrack(IPCbuffer* bfr, int track, int side, const std::vector<uint8_t>& stream)
{
    uint32_t maxDataSize = bfr->maxDataSize;        // IPCbuffer is packed - copy fields before REQUIRE binds them
    REQUIRE(2 + stream.size() <= maxDataSize);

    bfr->data[0] = track;
    bfr->data[1] = side;
    memcpy(bfr->data + 2, stream.data(), stream.size());
    bfr->length = 2 + stream.size();
    bfr->trackNo = track;
    bfr->sideNo = side;

    storeReceivedTrack(bfr);
}

struct TrackReceiveFixture {
    TrackReceiveFixture() {
        fakeReset();
        ipcInit();

        hwPosition.track = 0;
        memset(trackData0, 0, sizeof(trackData0));
        memset(trackData1, 0, sizeof(trackData1));
    }
};

}   // namespace

TEST_SUITE("fdd track receive") {

TEST_CASE_FIXTURE(TrackReceiveFixture, "shorter track after a longer one in the same IPC buffer keeps nothing of the longer one") {
    // e.g. image with 10 sectors per track, then image with 9 - the old sector 10 must not be streamed
    hwPosition.track = 3;
    IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);
    REQUIRE(bfr != NULL);

    receiveTrack(bfr, 3, 0, makeStream(12500, 0x5a));

    std::vector<uint8_t> shortStream = makeStream(11200, 0x66);
    receiveTrack(bfr, 3, 0, shortStream);

    CHECK(psramTrack(3, 0) == wholeTrack(shortStream));
    CHECK(memcmp(trackData0, wholeTrack(shortStream).data(), READTRACKDATA_SIZE_BYTES) == 0);
}

TEST_CASE_FIXTURE(TrackReceiveFixture, "stream is stored without the track + side prefix") {
    hwPosition.track = 7;
    std::vector<uint8_t> stream = makeStream(9000, 0x9d);
    stream[0] = 0x12;       // stream table starts right at offset 0
    stream[1] = 0x34;

    receiveTrack(ipcGetFreeBuffer(1, 0), 7, 1, stream);

    std::vector<uint8_t> t = psramTrack(7, 1);
    CHECK(t[0] == 0x12);
    CHECK(t[1] == 0x34);
    CHECK(t == wholeTrack(stream));
    CHECK(memcmp(trackData1, t.data(), READTRACKDATA_SIZE_BYTES) == 0);   // side 1 buffer
}

TEST_CASE_FIXTURE(TrackReceiveFixture, "full size message stores its stream and reads nothing beyond the IPC buffer") {
    // 2 + 13798 = 13800 B, the whole IPC buffer - used to read 2 B past it. Take the last buffer, so an over-read
    // leaves the dataToCore1 array (AddressSanitizer reports that, see 'make test-asan').
    IPCbuffer* bfr = NULL;
    for (int i = 0; i < IPC_BUFFER_LARGE_COUNT; i++) {
        bfr = ipcGetFreeBuffer(1, 0);
    }
    REQUIRE(bfr != NULL);
    uint32_t maxDataSize = bfr->maxDataSize;
    REQUIRE(maxDataSize == READTRACKDATA_SIZE_BYTES);

    hwPosition.track = 5;
    std::vector<uint8_t> stream = makeStream(READTRACKDATA_SIZE_BYTES - 2, 0xe7);
    receiveTrack(bfr, 5, 0, stream);

    CHECK(psramTrack(5, 0) == wholeTrack(stream));
    CHECK(memcmp(trackData0, wholeTrack(stream).data(), READTRACKDATA_SIZE_BYTES) == 0);
}

TEST_CASE_FIXTURE(TrackReceiveFixture, "track that isn't streamed now goes only to PSRAM") {
    hwPosition.track = 10;
    std::vector<uint8_t> before(trackData0, trackData0 + READTRACKDATA_SIZE_BYTES);
    std::vector<uint8_t> stream = makeStream(8000, 0xb6);

    receiveTrack(ipcGetFreeBuffer(1, 0), 11, 0, stream);

    CHECK(psramTrack(11, 0) == wholeTrack(stream));
    CHECK(memcmp(trackData0, before.data(), READTRACKDATA_SIZE_BYTES) == 0);
}

TEST_CASE_FIXTURE(TrackReceiveFixture, "message too short to hold even the prefix stores an empty track") {
    hwPosition.track = 2;
    psramStoreTrack(2, 0, std::vector<uint8_t>(READTRACKDATA_SIZE_BYTES, 0x55).data());

    IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);
    bfr->data[0] = 2;
    bfr->length = 1;
    bfr->trackNo = 2;
    bfr->sideNo = 0;
    storeReceivedTrack(bfr);

    CHECK(psramTrack(2, 0) == std::vector<uint8_t>(READTRACKDATA_SIZE_BYTES, 0));
}

}
