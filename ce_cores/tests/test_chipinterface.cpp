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

//----------------------------------------------------------------------
// accepting connections - a reconnecting device's old connections are closed

#include <netinet/in.h>
#include <arpa/inet.h>

void onClientRequest(sockaddr_in *clientAddr, uint8_t *recvData, int len);     // from discovery/discovery.cpp

// listening ChipInterface on a free local port, devices connect from 127.0.0.x addresses
struct ChipServerHarness {
    ChipInterface  *ci;
    uint16_t        port;
    std::vector<int> deviceSockets;

    ChipServerHarness(int whichLog) {
        ci = NULL;
        for(int i=0; i<50 && !ci; i++) {               // find free port
            port = 40000 + ((getpid() * 7 + i * 131) % 20000);
            ChipInterface *c = new ChipInterface(whichLog, NET_ATN_HANS_ID, SYNC_TAG_HDD, port);
            if(c->ciOpen()) {
                ci = c;
            } else {
                delete c;
            }
        }
        REQUIRE(ci != NULL);
    }

    ~ChipServerHarness() {
        ci->ciClose();                                  // closes listening socket and all clients
        delete ci;
        for(int fd : deviceSockets) {
            close(fd);
        }
    }

    // device with this source IP connects, server accepts it; returns client index
    int deviceConnects(const char *srcIp, int &deviceFd) {
        deviceFd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(deviceFd >= 0);
        deviceSockets.push_back(deviceFd);

        sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        inet_pton(AF_INET, srcIp, &src.sin_addr);
        REQUIRE(bind(deviceFd, (sockaddr *) &src, sizeof(src)) == 0);

        sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
        REQUIRE(connect(deviceFd, (sockaddr *) &dst, sizeof(dst)) == 0);

        int idx = FD_EMPTY;
        for(int i=0; i<100 && idx == FD_EMPTY; i++) {
            idx = ci->acceptSocketIfNeededAndPossible();
            if(idx == FD_EMPTY) {
                usleep(2000);
            }
        }
        REQUIRE(idx != FD_EMPTY);
        return idx;
    }
};

static void registerMac(const char *ip, uint8_t macLastByte)     // like discovery request from device: "CELC" + mac
{
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr.sin_addr);

    uint8_t req[10] = {'C', 'E', 'L', 'C', 0x02, 0x00, 0x00, 0x00, 0x00, macLastByte};
    onClientRequest(&addr, req, sizeof(req));
}

static bool deviceSeesClosed(int deviceFd)      // server closed connection -> device reads EOF
{
    char c;
    struct timeval tv = {1, 0};
    setsockopt(deviceFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(deviceFd, &c, 1, 0) == 0;
}

TEST_CASE("chipinterface: device reconnecting from the same IP - old connection is closed (HDD / IKBD too)")
{
    ChipServerHarness h(LOGFILE_HDD);                       // used to happen only for FDD

    int oldFd, newFd, otherFd;
    int oldIdx = h.deviceConnects("127.0.0.11", oldFd);
    int otherIdx = h.deviceConnects("127.0.0.12", otherFd); // another device
    int newIdx = h.deviceConnects("127.0.0.11", newFd);     // first device reconnects (e.g. after reboot)

    CHECK(newIdx != oldIdx);
    CHECK(h.ci->clientGetByIndex(oldIdx)->fdClient == FD_EMPTY);     // old connection closed right away
    CHECK(h.ci->clientGetByIndex(newIdx)->fdClient != FD_EMPTY);
    CHECK(h.ci->clientGetByIndex(otherIdx)->fdClient != FD_EMPTY);   // other device untouched
    CHECK(deviceSeesClosed(oldFd));
}

TEST_CASE("chipinterface: device reconnecting with a different IP but same MAC - old connection is closed")
{
    registerMac("127.0.0.21", 0x21);                    // device got IP .21 ...
    registerMac("127.0.0.22", 0x21);                    // ... then (DHCP) .22 - same MAC
    registerMac("127.0.0.23", 0x23);                    // other device

    ChipServerHarness h(LOGFILE_IKBD);

    int oldFd, newFd, otherFd;
    int oldIdx = h.deviceConnects("127.0.0.21", oldFd);
    int otherIdx = h.deviceConnects("127.0.0.23", otherFd);
    int newIdx = h.deviceConnects("127.0.0.22", newFd);

    CHECK(h.ci->clientGetByIndex(oldIdx)->fdClient == FD_EMPTY);
    CHECK(h.ci->clientGetByIndex(newIdx)->fdClient != FD_EMPTY);
    CHECK(h.ci->clientGetByIndex(otherIdx)->fdClient != FD_EMPTY);
    CHECK(h.ci->clientGetByIndex(newIdx)->mac[5] == 0x21);          // mac known right after accept
    CHECK(deviceSeesClosed(oldFd));
}

TEST_CASE("chipinterface: reconnecting floppy device keeps its floppy slot, other device gets another one")
{
    ChipServerHarness h(LOGFILE_FDD);

    int fdA, fdB, fdA2;
    int idxA = h.deviceConnects("127.0.0.31", fdA);
    int slotA = h.ci->clientGetByIndex(idxA)->floppySlotIndex;
    int idxB = h.deviceConnects("127.0.0.32", fdB);
    int slotB = h.ci->clientGetByIndex(idxB)->floppySlotIndex;
    CHECK(slotA != slotB);

    int idxA2 = h.deviceConnects("127.0.0.31", fdA2);   // device A reconnects
    CHECK(h.ci->clientGetByIndex(idxA)->fdClient == FD_EMPTY);
    CHECK(h.ci->clientGetByIndex(idxA2)->floppySlotIndex == slotA);
    CHECK(h.ci->clientGetByIndex(idxB)->floppySlotIndex == slotB);
}
