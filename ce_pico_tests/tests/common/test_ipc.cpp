// ipc.cpp - buffer pools and fifos between the two cores

#include "test_helpers.h"

TEST_SUITE("ipc") {

TEST_CASE_FIXTURE(FirmwareFixture, "ipcInit makes all buffers free and indexed") {
    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        CHECK(buffersForCore0[i].free);
        CHECK(buffersForCore1[i].free);
        CHECK(buffersForCore0[i].index == i);
        CHECK(buffersForCore1[i].index == i);
    }

    CHECK(ipcGetBufferFromFifo(0) == nullptr);
    CHECK(ipcGetBufferFromFifo(1) == nullptr);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ipcGetFreeBuffer hands out each buffer once, then NULL after the wait") {
    IPCbuffer *got[IPC_BUFFER_COUNT];

    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        got[i] = ipcGetFreeBuffer(0, 10);
        REQUIRE(got[i] != nullptr);
        CHECK_FALSE(got[i]->free);

        for (int j = 0; j < i; j++) {
            CHECK(got[j] != got[i]);
        }
    }

    fakeMillisStep = 1;                 // let the busy wait time out
    uint32_t start = fakeNow;
    CHECK(ipcGetFreeBuffer(0, 10) == nullptr);
    CHECK(fakeNow - start >= 10);

    ipcReleaseBuffer(got[5]);           // released by the other core
    CHECK(got[5]->free);
    CHECK(ipcGetFreeBuffer(0, 10) == got[5]);
}

TEST_CASE_FIXTURE(FirmwareFixture, "taken buffer goes through the fifo and back to the pool") {
    IPCbuffer *b = ipcGetFreeBuffer(1, 0);
    REQUIRE(b != nullptr);
    uint8_t v = 0x42;
    ipcSetBufferAndPutToFifo(b, 1, STATE_READ_STATUS, 1, &v, 1);
    CHECK_FALSE(b->free);

    IPCbuffer *r = ipcGetBufferFromFifo(1);
    REQUIRE(r == b);
    CHECK_FALSE(r->free);               // still owned while being handled

    ipcReleaseBuffer(r);
    CHECK(r->free);
    CHECK(ipcGetFreeBuffer(1, 0) == b); // first free one again
}

TEST_CASE_FIXTURE(FirmwareFixture, "the two cores have separate pools") {
    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        REQUIRE(ipcGetFreeBuffer(0, 0) != nullptr);
    }

    IPCbuffer *b = ipcGetFreeBuffer(1, 0);
    REQUIRE(b != nullptr);
    CHECK(b == &buffersForCore1[0]);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ipcSetBufferAndPutToFifo stores fields and delivers in FIFO order") {
    uint8_t d1[3] = { 1, 2, 3 };
    uint8_t d2[1] = { 9 };

    IPCbuffer *a = ipcGetFreeBuffer(1, 0);
    IPCbuffer *b = ipcGetFreeBuffer(1, 0);
    ipcSetBufferAndPutToFifo(a, 1, STATE_DATA_READ, 3, d1, 3);
    ipcSetBufferAndPutToFifo(b, 1, STATE_READ_STATUS, 1, d2, 1);

    CHECK(ipcGetBufferFromFifo(0) == nullptr);      // nothing went to the other core

    IPCbuffer *r = ipcGetBufferFromFifo(1);
    REQUIRE(r == a);
    CHECK(r->command == STATE_DATA_READ);
    CHECK((uint32_t) r->length == 3);
    CHECK(memcmp(r->data, d1, 3) == 0);
    CHECK_FALSE(r->free);                           // receiver frees it after handling

    r = ipcGetBufferFromFifo(1);
    REQUIRE(r == b);
    CHECK(r->command == STATE_READ_STATUS);
    CHECK(r->data[0] == 9);

    CHECK(ipcGetBufferFromFifo(1) == nullptr);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ipcSetBufferAndPutToFifo without data keeps length, does not copy") {
    IPCbuffer *a = ipcGetFreeBuffer(0, 0);
    memset(a->data, 0x77, IPC_BUFFER_SIZE);

    ipcSetBufferAndPutToFifo(a, 0, STATE_SEND_WRITE_MORE_DATA, 123456, nullptr, 0);

    IPCbuffer *r = ipcGetBufferFromFifo(0);
    REQUIRE(r == a);
    CHECK((uint32_t) r->length == 123456);                     // length may be bigger than the buffer, it's just a value
    CHECK(r->data[0] == 0x77);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ipcSetBufferAndPutToFifo copies at most IPC_BUFFER_SIZE bytes") {
    std::vector<uint8_t> big(IPC_BUFFER_SIZE + 100, 0xab);

    IPCbuffer *a = &buffersForCore0[0];
    a->free = false;
    ipcSetBufferAndPutToFifo(a, 0, STATE_DATA_WRITE, big.size(), big.data(), big.size());

    CHECK(a->data[IPC_BUFFER_SIZE - 1] == 0xab);
    CHECK(buffersForCore0[1].index == 1);           // the next buffer is untouched
    CHECK(buffersForCore0[1].free);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ipcGetBufferFromFifo rejects out of range index") {
    ipcPutBufferToFifo(0, IPC_BUFFER_COUNT);
    CHECK(ipcGetBufferFromFifo(0) == nullptr);
    CHECK(ipcGetBufferFromFifo(0) == nullptr);      // and it was consumed
}

TEST_CASE_FIXTURE(FirmwareFixture, "fifo holds all IPC_BUFFER_COUNT buffers of a pool") {
    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        IPCbuffer *b = ipcGetFreeBuffer(0, 0);
        REQUIRE(b != nullptr);
        uint8_t v = i;
        ipcSetBufferAndPutToFifo(b, 0, STATE_DATA_WRITE, 1, &v, 1);
    }

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == IPC_BUFFER_COUNT);
    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        CHECK(msgs[i].data[0] == i);
    }
}

}
