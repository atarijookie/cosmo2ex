/*
    Tests of the FDD device settings sync (beep on step, device IKBD) - ChipInterface::fdd_syncDeviceSettings() and
    fdd_sendDeviceSettings(): the device reports its settings in the FW report, the web UI changes them in the per-device
    settings and the FDD core sends them to the device.
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
#include "chipinterface/chipinterface.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

namespace {

uint8_t deviceMac[6] = {0x2c, 0xcf, 0x67, 0xc4, 0xb1, 0xe5};

// empty temp dir as SETTINGS_DIR, removed at the end of the test
struct TempSettingsDir {
    std::string dir;

    TempSettingsDir() {
        char tmpl[] = "/tmp/ce_test_fddsettings_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        dotEnv["SETTINGS_DIR"] = dir;
    }

    ~TempSettingsDir() {
        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }
};

// one connected FDD device on a socketpair: CE side is client 0, the test is the device
struct FddDevice {
    TempSettingsDir settingsDir;
    ChipInterface  *ci;
    ClientInfo     *client;
    int             sock[2];        // [0] = device side (test), [1] = CE side

    FddDevice() {
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock) == 0);

        struct timeval tv = {0, 200 * 1000};
        setsockopt(sock[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ci = new ChipInterface(LOGFILE_FDD, NET_ATN_FRANZ_ID, SYNC_TAG_FDD, 0);
        client = ci->clientGetByIndex(0);
        client->fdClient = sock[1];
        client->bufReader.setFd(sock[1]);
    }

    ~FddDevice() {
        client->fdClient = FD_EMPTY;
        delete ci;
        close(sock[0]);
        close(sock[1]);
    }

    // FW report as ce_pico_fdd sends it, then what the FDD core does with it; flags -1 = older firmware (no flags)
    void fwReport(int flags) {
        std::vector<uint8_t> data = {0xf0, 0x25, 0x09, 0x01, DEV_FEATURE_FDD | DEV_FEATURE_IKBD, 0};
        data.insert(data.end(), deviceMac, deviceMac + 6);
        if(flags >= 0) {
            data.push_back((uint8_t) flags);
            data.push_back(0);
        }

        uint8_t head[10];
        Utils::storeDword(head + 0, SYNC_TAG_FDD);
        Utils::storeWord (head + 4, ATN_FW_VERSION);
        Utils::storeDword(head + 6, data.size());
        REQUIRE(write(sock[0], head, 10) == 10);
        REQUIRE(write(sock[0], data.data(), data.size()) == (ssize_t) data.size());

        REQUIRE(client->bufReader.waitForAtn(ATN_ANY, 1000) == NET_ATN_FRANZ_ID);
        client->bufReader.clear();              // header taken, as ChipInterface::waitForAtn() does
        ci->getFWversionFdd(client);
        ci->fdd_syncDeviceSettings(client);
    }

    // settings flags the device received (ATN_DEVICE_SETTINGS), -1 = nothing received
    int receivedSettings(void) {
        struct pollfd p = {sock[0], POLLIN, 0};
        if(poll(&p, 1, 100) != 1) {
            return -1;
        }

        uint8_t msg[11];
        REQUIRE(read(sock[0], msg, 11) == 11);
        CHECK(Utils::getDword(msg) == SYNC_TAG_FDD);
        CHECK(Utils::getWord(msg + 4) == ATN_DEVICE_SETTINGS);
        CHECK(Utils::getDword(msg + 6) == 1);
        return msg[10];
    }

    // what the web UI does when a checkbox is changed: store the settings, mark them pending, ask the core to send them
    void webUiChange(bool ikbd, bool beep, bool deviceConnected) {
        Settings s(deviceMac);
        s.setBool(FDD_SETTING_IKBD_ENABLED, ikbd);
        s.setBool(FDD_SETTING_BEEP_ON_STEP, beep);
        s.setBool(FDD_SETTING_PENDING, true);

        if(deviceConnected) {
            ci->fdd_sendDeviceSettings(client);
        }
    }

    bool stored(const char* key) {
        Settings s(deviceMac);
        return s.getBool(key, false);
    }

    bool isStored(const char* key) {
        Settings s(deviceMac);
        return s.getInt(key, -1) != -1;
    }
};

const int BOTH = DEV_SETTING_IKBD | DEV_SETTING_BEEP_ON_STEP;

}   // namespace

TEST_CASE("fdd settings: older firmware without settings flags - nothing stored, nothing sent")
{
    FddDevice d;
    d.fwReport(-1);

    CHECK(d.client->deviceSettings == -1);
    CHECK_FALSE(d.isStored(FDD_SETTING_IKBD_ENABLED));
    CHECK_FALSE(d.isStored(FDD_SETTING_BEEP_ON_STEP));
    CHECK(d.receivedSettings() == -1);
}

TEST_CASE("fdd settings: on connect the device's settings are stored")
{
    FddDevice d;
    d.fwReport(DEV_SETTING_IKBD);

    CHECK(d.stored(FDD_SETTING_IKBD_ENABLED));
    CHECK_FALSE(d.stored(FDD_SETTING_BEEP_ON_STEP));
    CHECK(d.receivedSettings() == -1);
}

TEST_CASE("fdd settings: changed in the captive portal - the new settings are stored")
{
    FddDevice d;
    d.fwReport(BOTH);
    d.fwReport(DEV_SETTING_IKBD);                   // beep on step switched off on the device

    CHECK(d.stored(FDD_SETTING_IKBD_ENABLED));
    CHECK_FALSE(d.stored(FDD_SETTING_BEEP_ON_STEP));
}

TEST_CASE("fdd settings: changed in the web UI - sent to the device, a report with the old value doesn't undo it")
{
    FddDevice d;
    d.fwReport(BOTH);

    d.webUiChange(true, false, true);
    CHECK(d.receivedSettings() == DEV_SETTING_IKBD);
    CHECK_FALSE(d.stored(FDD_SETTING_PENDING));

    d.fwReport(BOTH);                               // sent before the device got the change
    CHECK_FALSE(d.stored(FDD_SETTING_BEEP_ON_STEP));

    d.fwReport(DEV_SETTING_IKBD);                   // the device applied it
    CHECK(d.stored(FDD_SETTING_IKBD_ENABLED));
    CHECK_FALSE(d.stored(FDD_SETTING_BEEP_ON_STEP));
    CHECK(d.receivedSettings() == -1);              // nothing more sent
}

TEST_CASE("fdd settings: changed in the web UI while the device wasn't connected - sent on connect, not overwritten")
{
    FddDevice d;
    d.webUiChange(false, false, false);

    d.fwReport(BOTH);                               // device connects with its old settings
    CHECK(d.receivedSettings() == 0);
    CHECK_FALSE(d.stored(FDD_SETTING_IKBD_ENABLED));
    CHECK_FALSE(d.stored(FDD_SETTING_BEEP_ON_STEP));
    CHECK_FALSE(d.stored(FDD_SETTING_PENDING));

    d.fwReport(0);                                  // the device applied them
    CHECK_FALSE(d.stored(FDD_SETTING_IKBD_ENABLED));
    CHECK(d.receivedSettings() == -1);
}

TEST_CASE("fdd settings: nothing stored for the device yet - both settings default to on when sent")
{
    FddDevice d;
    d.fwReport(-1);                                 // the mac is known now
    CHECK(d.ci->fdd_sendDeviceSettings(d.client));
    CHECK(d.receivedSettings() == BOTH);
}

//-------------------------------------------------------------------------------------------------
// LED colors - global settings (LED_COLOR_SETTING_*), sent to HDD (CMD_LED_COLORS) and FDD (ATN_LED_COLORS) devices

namespace {

// the message the device got: cmd code + payload, cmd -1 = nothing received
int receivedMsg(int sock, std::vector<uint8_t>& payload)
{
    struct pollfd p = {sock, POLLIN, 0};
    if(poll(&p, 1, 100) != 1) {
        return -1;
    }

    uint8_t head[10];
    REQUIRE(read(sock, head, 10) == 10);
    uint32_t len = Utils::getDword(head + 6);
    payload.resize(len);
    REQUIRE(read(sock, payload.data(), len) == (ssize_t) len);
    return Utils::getWord(head + 4);
}

}   // namespace

TEST_CASE("led colors: defaults when not stored or garbage")
{
    TempSettingsDir dir;
    Settings s;

    CHECK(ChipInterface::ledColorFromSettings(LED_COLOR_SETTING_FDD, LED_COLOR_DEFAULT_FDD) == LED_COLOR_DEFAULT_FDD);

    for(const char* bad : {"garbage", "00300", "0030000", "000000", "00300g"}) {
        s.setString(LED_COLOR_SETTING_FDD, bad);
        CHECK(ChipInterface::ledColorFromSettings(LED_COLOR_SETTING_FDD, LED_COLOR_DEFAULT_FDD) == LED_COLOR_DEFAULT_FDD);
    }

    s.setString(LED_COLOR_SETTING_FDD, "a0B0c0");
    CHECK(ChipInterface::ledColorFromSettings(LED_COLOR_SETTING_FDD, LED_COLOR_DEFAULT_FDD) == 0xa0b0c0);
}

TEST_CASE("led colors: FDD gets its color once per connection, again when changed")
{
    FddDevice d;
    std::vector<uint8_t> payload;

    d.ci->sendLedColorsIfNotSent(d.client, false);      // nothing stored - the defaults
    REQUIRE(receivedMsg(d.sock[0], payload) == ATN_LED_COLORS);
    CHECK(payload == std::vector<uint8_t>({0x00, 0x30, 0x00}));

    d.ci->sendLedColorsIfNotSent(d.client, false);      // already sent on this connection
    CHECK(receivedMsg(d.sock[0], payload) == -1);

    Settings s;
    s.setString(LED_COLOR_SETTING_FDD, "102030");
    d.ci->sendLedColorsToAll(false);                    // changed in the web UI
    REQUIRE(receivedMsg(d.sock[0], payload) == ATN_LED_COLORS);
    CHECK(payload == std::vector<uint8_t>({0x10, 0x20, 0x30}));
}

TEST_CASE("led colors: HDD gets ACSI + SCSI color")
{
    FddDevice d;                                        // the socket part is the same for HDD
    std::vector<uint8_t> payload;

    Settings s;
    s.setString(LED_COLOR_SETTING_ACSI, "400000");      // SCSI not stored - default

    CHECK(d.ci->sendLedColors(d.client, true));
    REQUIRE(receivedMsg(d.sock[0], payload) == CMD_LED_COLORS);
    CHECK(payload == std::vector<uint8_t>({0x40, 0x00, 0x00, 0x00, 0x00, 0x40}));
    CHECK(d.client->ledColorsSent);
}
