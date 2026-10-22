// written_sectors.cpp - core0 sends the sectors written by the ST (captured by core1) to the host. A write done while not
// connected must not be lost: it's kept and sent after reconnect, in the order of writing, before the device asks for the
// whole image again (the main loop doesn't ask while writtenSectorsPending()).

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_connection.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "written_sectors.h"

extern IPCbuffer buffersForCore0[IPC_BUFFER_SMALL_COUNT];

namespace {

// what core1 does on write end: written sector message (header, side + track, sector, data) into the fifo for core0
IPCbuffer* coreOneWritesSector(uint8_t track, uint8_t sector, uint8_t fill)
{
    IPCbuffer* bfr = ipcGetFreeBuffer(0, 0);
    REQUIRE(bfr != NULL);

    storeHeader(bfr->data, ATN_SECTOR_WRITTEN, 12);
    bfr->data[10] = track;
    bfr->data[11] = sector;
    memset(bfr->data + 12, fill, 700);

    ipcSetBufferAndPutToFifo(bfr, 0, CMD_SEND_WRITTEN_SECTOR, 12 + 700);
    return bfr;
}

// the sector numbers of the sent messages, in sending order
std::vector<int> sentSectors(void)
{
    std::vector<int> s;
    for (const auto& msg : fakeHostSent) {
        s.push_back(msg[11]);
    }
    return s;
}

int freeBuffers(void)
{
    int n = 0;
    for (int i = 0; i < IPC_BUFFER_SMALL_COUNT; i++) {
        n += buffersForCore0[i].free ? 1 : 0;
    }
    return n;
}

struct WrittenSectorsFixture {
    WrittenSectorsFixture() {
        fakeReset();
        ipcInit();
        writtenSectorsReset();
        fakeHostReset();
    }
};

}   // namespace

TEST_SUITE("fdd written sectors to host") {

TEST_CASE_FIXTURE(WrittenSectorsFixture, "connected - written sector is sent whole and its buffer freed") {
    IPCbuffer* bfr = coreOneWritesSector(5, 3, 0x6b);

    writtenSectorsHandle();

    REQUIRE(fakeHostSent.size() == 1);
    const std::vector<uint8_t>& msg = fakeHostSent[0];
    CHECK(msg.size() == 12 + 700);
    CHECK(getDword((uint8_t*) msg.data()) == SYNC_TAG_FDD);
    CHECK(getWord((uint8_t*) msg.data() + 4) == ATN_SECTOR_WRITTEN);
    CHECK(getDword((uint8_t*) msg.data() + 6) == 2 + 700);     // length after the header
    CHECK(msg[10] == 5);
    CHECK(msg[11] == 3);
    CHECK(msg[12] == 0x6b);
    CHECK(msg.back() == 0x6b);

    CHECK(bfr->free);
    CHECK_FALSE(writtenSectorsPending());
}

TEST_CASE_FIXTURE(WrittenSectorsFixture, "not connected - written sector is kept and sent after reconnect") {
    fakeHostConnected = false;
    IPCbuffer* bfr = coreOneWritesSector(5, 3, 0x6b);

    for (int i = 0; i < 10; i++) {              // main loop runs on while disconnected
        writtenSectorsHandle();
    }

    CHECK(fakeHostSent.empty());
    CHECK_FALSE(bfr->free);                     // used to be freed = write lost
    CHECK(writtenSectorsPending());             // main loop won't ask for the whole image yet

    fakeHostConnected = true;
    writtenSectorsHandle();

    CHECK(sentSectors() == std::vector<int>{ 3 });
    CHECK(bfr->free);
    CHECK_FALSE(writtenSectorsPending());       // now the image can be asked for
}

TEST_CASE_FIXTURE(WrittenSectorsFixture, "writes kept while not connected are sent in the order they were written") {
    fakeHostConnected = false;
    coreOneWritesSector(1, 4, 0x11);
    writtenSectorsHandle();
    coreOneWritesSector(1, 2, 0x22);
    coreOneWritesSector(0, 9, 0x33);
    writtenSectorsHandle();

    fakeHostConnected = true;
    coreOneWritesSector(2, 1, 0x44);            // one more after reconnect - goes after the kept ones
    writtenSectorsHandle();

    CHECK(sentSectors() == std::vector<int>{ 4, 2, 9, 1 });
    CHECK(fakeHostSent[0][12] == 0x11);
    CHECK(fakeHostSent[3][12] == 0x44);
    CHECK(freeBuffers() == IPC_BUFFER_SMALL_COUNT);
}

TEST_CASE_FIXTURE(WrittenSectorsFixture, "connection lost in the middle - the rest is sent later, nothing twice") {
    coreOneWritesSector(1, 1, 0x11);
    fakeHostConnected = true;
    writtenSectorsHandle();                     // first one sent
    fakeHostConnected = false;
    coreOneWritesSector(1, 2, 0x22);
    coreOneWritesSector(1, 3, 0x33);
    writtenSectorsHandle();                     // these two can't be sent
    CHECK(sentSectors() == std::vector<int>{ 1 });

    fakeHostConnected = true;
    writtenSectorsHandle();
    CHECK(sentSectors() == std::vector<int>{ 1, 2, 3 });
    CHECK(freeBuffers() == IPC_BUFFER_SMALL_COUNT);
}

TEST_CASE_FIXTURE(WrittenSectorsFixture, "send stopped halfway - connection closed, the whole message sent again after reconnect") {
    // the host already got the start of the message - on the same connection it would take the start of the resent
    // message as the rest of the first one, so the connection is closed and the message sent whole on the new one
    coreOneWritesSector(3, 5, 0x55);
    fakeHostSendStopsAfter = 300;
    writtenSectorsHandle();

    CHECK(fakeHostSentPartial.size() == 1);
    CHECK(fakeHostSent.empty());
    CHECK_FALSE(fakeHostConnected);
    CHECK(writtenSectorsPending());

    writtenSectorsHandle();                     // still not connected - nothing more goes out
    CHECK(fakeHostSent.empty());

    fakeHostConnected = true;                   // reconnected
    writtenSectorsHandle();

    REQUIRE(fakeHostSent.size() == 1);          // whole, from its header, once
    CHECK(getDword(fakeHostSent[0].data()) == SYNC_TAG_FDD);
    CHECK(fakeHostSent[0].size() == 12 + 700);
    CHECK(fakeHostSent[0][11] == 5);
    CHECK_FALSE(writtenSectorsPending());
    CHECK(freeBuffers() == IPC_BUFFER_SMALL_COUNT);
}

TEST_CASE_FIXTURE(WrittenSectorsFixture, "kept writes are limited by the core0 IPC buffers - all of them are sent after reconnect") {
    fakeHostConnected = false;
    for (int i = 0; i < IPC_BUFFER_SMALL_COUNT; i++) {
        coreOneWritesSector(i / 9, 1 + i % 9, i);
        writtenSectorsHandle();
    }

    CHECK(freeBuffers() == 0);                  // core1 has no buffer for one more write until these are sent
    fakeMillisStep = 1;                         // let the wait for a buffer time out
    CHECK(ipcGetFreeBuffer(0, 0) == NULL);

    fakeHostConnected = true;
    writtenSectorsHandle();

    REQUIRE(fakeHostSent.size() == IPC_BUFFER_SMALL_COUNT);
    for (int i = 0; i < IPC_BUFFER_SMALL_COUNT; i++) {
        CAPTURE(i);
        CHECK(fakeHostSent[i][12] == i);        // in order
    }
    CHECK(freeBuffers() == IPC_BUFFER_SMALL_COUNT);
}

}
