/*
    Tests of the firmware update over the device connection - FwUpdateSender (host side of ce_pico_hdd/fw_update.cpp)
    and ChipInterface::storeFwInfo(). The test plays the device on a socketpair.
*/
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/debug.h"
#include "misc/utils.h"
#include "misc/settings.h"
#include "misc/json.h"
#include "chipinterface/chipinterface.h"
#include "chipinterface/fwupdatesender.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

using json = nlohmann::json;

namespace {

uint8_t deviceMac[6] = {0x02, 0xce, 0x00, 0x11, 0x22, 0x33};
const char* deviceMacStr = "02CE00112233";
const char* md5Hex = "00112233445566778899aabbccddeeff";

struct TempDir {
    std::string dir;

    TempDir() {
        char tmpl[] = "/tmp/ce_test_fwupdate_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        dotEnv["SETTINGS_DIR"] = dir;
    }

    ~TempDir() {
        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }
};

struct Command {
    uint16_t cmd = 0;
    std::vector<uint8_t> data;
};

// one connected HDD device on a socketpair: CE side is client 0, the test is the device
struct HddDevice {
    TempDir         tmp;
    ChipInterface  *ci;
    ClientInfo     *client;
    FwUpdateSender *sender;
    int             sock[2];        // [0] = device side (test), [1] = CE side

    HddDevice() {
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock) == 0);

        struct timeval tv = {0, 200 * 1000};
        setsockopt(sock[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ci = new ChipInterface(LOGFILE_HDD, NET_ATN_HANS_ID, SYNC_TAG_HDD, 0);
        client = ci->clientGetByIndex(0);
        client->fdClient = sock[1];
        client->bufReader.setFd(sock[1]);
        memcpy(client->mac, deviceMac, 6);

        sender = new FwUpdateSender(ci, LOGFILE_HDD, CMD_FW_UPDATE_START, CMD_FW_UPDATE_DATA, CMD_FW_UPDATE_END, CMD_FW_UPDATE_ABORT);
    }

    ~HddDevice() {
        delete sender;
        client->fdClient = FD_EMPTY;
        delete ci;
        close(sock[0]);
        close(sock[1]);
    }

    // device sends ATN with data, host takes the header (as ChipInterface::actionNeeded() does)
    void deviceSends(uint16_t atn, const std::vector<uint8_t>& data) {
        uint8_t head[10];
        Utils::storeDword(head + 0, SYNC_TAG_HDD);
        Utils::storeWord (head + 4, atn);
        Utils::storeDword(head + 6, data.size());
        REQUIRE(write(sock[0], head, 10) == 10);
        REQUIRE(write(sock[0], data.data(), data.size()) == (ssize_t) data.size());

        REQUIRE(client->bufReader.waitForAtn(ATN_ANY, 1000) == NET_ATN_HANS_ID);
        client->bufReader.clear();
    }

    void deviceAnswers(uint16_t cmd, uint8_t result, uint32_t received, const char* msg = "") {
        std::vector<uint8_t> data(6, 0);
        data[0] = (uint8_t) cmd;
        data[1] = result;
        Utils::storeDword(data.data() + 2, received);
        data.insert(data.end(), msg, msg + strlen(msg) + 1);

        deviceSends(ATN_FW_UPDATE_STATUS, data);
        sender->onStatus(client);
    }

    // next command the device got, cmd 0 = nothing
    Command deviceReceives(void) {
        Command c;
        struct pollfd p = {sock[0], POLLIN, 0};
        if(poll(&p, 1, 200) != 1) {
            return c;
        }

        uint8_t head[10];
        REQUIRE(read(sock[0], head, 10) == 10);
        CHECK(Utils::getDword(head) == SYNC_TAG_HDD);
        c.cmd = Utils::getWord(head + 4);
        c.data.resize(Utils::getDword(head + 6));

        size_t got = 0;
        while(got < c.data.size()) {
            ssize_t n = read(sock[0], c.data.data() + got, c.data.size() - got);
            REQUIRE(n > 0);
            got += n;
        }
        return c;
    }

    json status(void) {
        Settings s(deviceMac);
        std::string st = s.getString(FW_UPDATE_STATUS_KEY, "{}");
        return json::parse(st);
    }

    std::string writeImage(size_t size) {
        std::vector<uint8_t> img(size);
        for(size_t i=0; i<size; i++) {
            img[i] = (uint8_t) (i * 7);
        }

        std::string path = tmp.dir + "/fw.bin";
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f != NULL);
        fwrite(img.data(), 1, size, f);
        fclose(f);
        return path;
    }
};

}   // namespace

TEST_CASE("fw update: firmware info from the device is stored to its settings")
{
    HddDevice dev;

    std::vector<uint8_t> data(30, 0);
    Utils::storeDword(data.data(), 102400);
    for(int i=0; i<16; i++) data[4 + i] = 0xa0 + i;
    Utils::storeDword(data.data() + 20, 0x180000);
    Utils::storeDword(data.data() + 24, 0x1ff000);
    data[28] = 1;
    const char* ident = "dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.0;";
    data.insert(data.end(), ident, ident + strlen(ident) + 1);
    strcpy(dev.client->fwVersion, "2026-11-14");

    dev.deviceSends(ATN_FW_INFO, data);
    dev.ci->storeFwInfo(dev.client);

    Settings s(deviceMac);
    CHECK(s.getString("FW_IDENT", "") == ident);
    CHECK(s.getString("FW_IMAGE_SIZE", "") == "102400");
    CHECK(s.getString("FW_IMAGE_MD5", "") == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF");
    CHECK(s.getString("FW_FS_START", "") == std::to_string(0x180000));
    CHECK(s.getString("FW_FS_END", "") == std::to_string(0x1ff000));
    CHECK(s.getString("FW_OTA", "") == "1");
    CHECK(s.getString("FW_VERSION", "") == "2026-11-14");
    CHECK(s.getString("FW_STATE", "") == "ok");

    data[28] = 1 | 4;           // rolled back to the previous firmware
    dev.deviceSends(ATN_FW_INFO, data);
    dev.ci->storeFwInfo(dev.client);
    CHECK(s.getString("FW_STATE", "") == "rolled_back");

    data[28] = 1 | 2;           // new firmware on trial
    dev.deviceSends(ATN_FW_INFO, data);
    dev.ci->storeFwInfo(dev.client);
    CHECK(s.getString("FW_STATE", "") == "trial");
}

TEST_CASE("fw update: the whole image is sent in chunks, each after the device confirmed the previous one")
{
    HddDevice dev;
    const size_t size = 2 * FWU_CHUNK_SIZE + 452;
    std::string file = dev.writeImage(size);

    dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);

    Command c = dev.deviceReceives();
    REQUIRE(c.cmd == CMD_FW_UPDATE_START);
    REQUIRE(c.data.size() == 20);
    CHECK(Utils::getDword(c.data.data()) == size);
    CHECK(c.data[4] == 0x00);
    CHECK(c.data[19] == 0xff);
    CHECK(dev.status()["state"] == "starting");
    CHECK(dev.sender->isRunning(0));

    dev.deviceAnswers(CMD_FW_UPDATE_START, 0, 0);

    uint32_t received = 0;
    std::vector<uint8_t> got;
    while(true) {
        c = dev.deviceReceives();
        if(c.cmd != CMD_FW_UPDATE_DATA) {
            break;
        }

        REQUIRE(c.data.size() >= 4);
        CHECK(Utils::getDword(c.data.data()) == received);
        CHECK(c.data.size() - 4 <= FWU_CHUNK_SIZE);
        got.insert(got.end(), c.data.begin() + 4, c.data.end());
        received += c.data.size() - 4;

        CHECK(dev.deviceReceives().cmd == 0);       // nothing more before the device answers
        dev.deviceAnswers(CMD_FW_UPDATE_DATA, 0, received);
    }

    REQUIRE(got.size() == size);
    for(size_t i=0; i<size; i++) {
        REQUIRE(got[i] == (uint8_t) (i * 7));
    }

    CHECK(c.cmd == CMD_FW_UPDATE_END);
    CHECK(dev.status()["state"] == "finishing");

    dev.deviceAnswers(CMD_FW_UPDATE_END, 0, received, "rebooting");
    CHECK(dev.status()["state"] == "rebooting");
    CHECK(dev.status()["progress"] == 100);
    CHECK_FALSE(dev.sender->isRunning(0));
}

TEST_CASE("fw update: device refuses the image - failed with its message")
{
    HddDevice dev;
    std::string file = dev.writeImage(1000);

    dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
    REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_START);
    dev.deviceAnswers(CMD_FW_UPDATE_START, 0, 0);
    REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_DATA);
    dev.deviceAnswers(CMD_FW_UPDATE_DATA, 0, 1000);
    REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_END);

    dev.deviceAnswers(CMD_FW_UPDATE_END, 6, 1000, "firmware is for FDD PICO-2W, this is HDD W6100-EVB-PICO");

    json st = dev.status();
    CHECK(st["state"] == "failed");
    CHECK(st["message"].get<std::string>().find("firmware is for FDD PICO-2W") != std::string::npos);
    CHECK_FALSE(dev.sender->isRunning(0));
}

TEST_CASE("fw update: device confirms a wrong amount - aborted")
{
    HddDevice dev;
    std::string file = dev.writeImage(3000);

    dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
    REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_START);
    dev.deviceAnswers(CMD_FW_UPDATE_START, 0, 0);
    REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_DATA);
    dev.deviceAnswers(CMD_FW_UPDATE_DATA, 0, 500);

    CHECK(dev.deviceReceives().cmd == CMD_FW_UPDATE_ABORT);
    CHECK(dev.status()["state"] == "failed");
}

TEST_CASE("fw update: not connected device, bad md5, missing file - not started")
{
    HddDevice dev;
    std::string file = dev.writeImage(1000);

    dev.sender->handleRequest("02CE00999999", file, md5Hex, false);
    CHECK(dev.deviceReceives().cmd == 0);

    dev.sender->handleRequest(deviceMacStr, file, "1234", false);
    CHECK(dev.deviceReceives().cmd == 0);
    CHECK(dev.status()["state"] == "failed");
    CHECK(dev.status()["message"] == "invalid MD5");

    dev.sender->handleRequest(deviceMacStr, dev.tmp.dir + "/nothing.bin", md5Hex, false);
    CHECK(dev.deviceReceives().cmd == 0);
    CHECK(dev.status()["state"] == "failed");
    CHECK_FALSE(dev.sender->isRunning(0));
}

TEST_CASE("fw update: abort from the web UI, lost connection")
{
    HddDevice dev;
    std::string file = dev.writeImage(5000);

    SUBCASE("abort") {
        dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
        REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_START);

        dev.sender->handleRequest(deviceMacStr, "", "", true);
        CHECK(dev.deviceReceives().cmd == CMD_FW_UPDATE_ABORT);
        CHECK(dev.status()["state"] == "aborted");
        CHECK_FALSE(dev.sender->isRunning(0));
    }

    SUBCASE("second request while running is refused") {
        dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
        REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_START);

        dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
        CHECK(dev.deviceReceives().cmd == 0);
        CHECK(dev.sender->isRunning(0));
    }

    SUBCASE("connection lost") {
        dev.sender->handleRequest(deviceMacStr, file, md5Hex, false);
        REQUIRE(dev.deviceReceives().cmd == CMD_FW_UPDATE_START);

        int fd = dev.client->fdClient;
        dev.client->fdClient = FD_EMPTY;        // as after clientsCloseOne()
        dev.sender->tick();
        dev.client->fdClient = fd;

        CHECK(dev.status()["state"] == "failed");
        CHECK(dev.status()["message"] == "connection to device lost");
        CHECK_FALSE(dev.sender->isRunning(0));
    }
}

TEST_CASE("fw update: without a file the image is FW_UPDATE_BIN in the device's settings dir")
{
    HddDevice dev;
    std::string file = dev.writeImage(1500);

    std::string dir = dev.tmp.dir + "/" + deviceMacStr;
    REQUIRE(system(("mkdir -p '" + dir + "' && mv '" + file + "' '" + dir + "/" FW_UPDATE_BIN_KEY "'").c_str()) == 0);

    dev.sender->handleRequest("02:ce:00:11:22:33", "", md5Hex, false);

    Command c = dev.deviceReceives();
    REQUIRE(c.cmd == CMD_FW_UPDATE_START);
    CHECK(Utils::getDword(c.data.data()) == 1500);
}
