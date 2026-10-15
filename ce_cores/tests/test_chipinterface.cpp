/*
    Tests of ChipInterface - receiving data blocks from a client (device) over TCP-like socket
*/
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/debug.h"
#include "misc/utils.h"
#include "chipinterface/chipinterface.h"

// one connected client (device) on a socketpair: CE side is registered as client 0, test writes as the device
struct ChipClientHarness {
    ChipInterface  *ci;
    ClientInfo     *client;
    int             sock[2];        // [0] = device side (test), [1] = CE side

    ChipClientHarness() {
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock) == 0);

        struct timeval tv = {0, 200 * 1000};       // like SO_RCVTIMEO set on accept, but shorter to keep test fast
        setsockopt(sock[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ci = new ChipInterface(LOGFILE_HDD, NET_ATN_HANS_ID, SYNC_TAG_HDD, 0);
        client = ci->clientGetByIndex(0);
        client->fdClient = sock[1];
        client->bufReader.setFd(sock[1]);
    }

    ~ChipClientHarness() {
        client->fdClient = FD_EMPTY;
        delete ci;
        if(sock[0] >= 0) {
            close(sock[0]);
        }
        close(sock[1]);
    }

    // device sends header saying that 'len' bytes of data follow - after this, the reader knows how much data to expect
    void deviceSendsHeader(uint32_t len) {
        uint8_t head[10];
        Utils::storeDword(head + 0, SYNC_TAG_HDD);
        Utils::storeWord (head + 4, ATN_WRITE_MORE_DATA);
        Utils::storeDword(head + 6, len);
        REQUIRE(write(sock[0], head, 10) == 10);
        REQUIRE(client->bufReader.waitForAtn(ATN_ANY, 1000) == NET_ATN_HANS_ID);
        REQUIRE(client->bufReader.dataSizeRest() == len);
    }
};

static std::vector<uint8_t> testData(int len)
{
    std::vector<uint8_t> data(len);
    for(int i=0; i<len; i++) {
        data[i] = (uint8_t) (i * 7 + 3);
    }
    return data;
}

TEST_CASE("chipinterface: data block arriving in many small pieces is received completely")
{
    ChipClientHarness h;
    h.deviceSendsHeader(512);
    std::vector<uint8_t> data = testData(512);

    // device sends the block in 16 pieces with small pauses (like a slow WiFi link) - used to give up after 3 recv() calls
    std::thread device([&]() {
        for(int i=0; i<16; i++) {
            write(h.sock[0], data.data() + i * 32, 32);
            usleep(3000);
        }
    });

    std::vector<uint8_t> got(512, 0);
    int cnt = h.ci->readRestOfData(h.client, got.data(), 512);
    device.join();

    CHECK(cnt == 512);
    CHECK(got == data);
    CHECK(h.client->bufReader.dataSizeRest() == 0);
}

TEST_CASE("chipinterface: receiving stops when device closes connection or goes silent")
{
    SUBCASE("device closes connection in the middle of the block") {
        ChipClientHarness h;
        h.deviceSendsHeader(512);
        std::vector<uint8_t> data = testData(100);
        REQUIRE(write(h.sock[0], data.data(), 100) == 100);
        close(h.sock[0]);
        h.sock[0] = -1;

        std::vector<uint8_t> got(512, 0);
        CHECK(h.ci->readRestOfData(h.client, got.data(), 512) == 100);     // gets what was sent, doesn't hang
    }

    SUBCASE("device sends part of the block and then nothing") {
        ChipClientHarness h;
        h.deviceSendsHeader(512);
        std::vector<uint8_t> data = testData(100);
        REQUIRE(write(h.sock[0], data.data(), 100) == 100);

        uint32_t start = Utils::getCurrentMs();
        std::vector<uint8_t> got(512, 0);
        CHECK(h.ci->readRestOfData(h.client, got.data(), 512) == 100);
        CHECK(Utils::elapsedMs(start) < RECV_FROM_CLIENT_TIMEOUT_MS + 1000);   // gives up on receive timeout, doesn't wait forever
    }
}
