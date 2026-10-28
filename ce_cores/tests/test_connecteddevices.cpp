/*
    Tests of ConnectedDevices - which devices have at least one connection to any core, written to CONNECTED_DEVICES_FILE
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <map>
#include <set>
#include <string>

#include "doctest.h"

#include "misc/global.h"
#include "misc/debug.h"
#include "misc/utils.h"
#include "misc/connecteddevices.h"
#include "chipinterface/chipinterface.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

// CONNECTED_DEVICES_FILE pointing to a temp file for the time of one test, registry cleared before and after
struct ConnectedFileHarness {
    std::string path;

    ConnectedFileHarness() {
        char tmpl[] = "/tmp/ce_test_connected_XXXXXX";
        int fd = mkstemp(tmpl);
        REQUIRE(fd >= 0);
        close(fd);
        path = tmpl;

        dotEnv["CONNECTED_DEVICES_FILE"] = path;
        ConnectedDevices::clear();
    }

    ~ConnectedFileHarness() {
        ConnectedDevices::clear();
        dotEnv.erase("CONNECTED_DEVICES_FILE");
        unlink(path.c_str());
    }

    std::string content() {
        std::string s;
        FILE *f = fopen(path.c_str(), "r");
        if(f) {
            char bfr[256];
            size_t n;
            while((n = fread(bfr, 1, sizeof(bfr), f)) > 0) {
                s.append(bfr, n);
            }
            fclose(f);
        }
        return s;
    }
};

TEST_CASE("connecteddevices: macToString")
{
    uint8_t mac[6] = {0x2c, 0xcf, 0x67, 0xc4, 0xb1, 0xe5};
    CHECK(ConnectedDevices::macToString(mac) == "2CCF67C4B1E5");     // same as the device dirs in SETTINGS_DIR

    uint8_t zeroMac[6] = {0};
    CHECK(ConnectedDevices::macToString(zeroMac) == "");            // unknown mac
}

TEST_CASE("connecteddevices: union of all cores, device stays connected while any core has it")
{
    ConnectedFileHarness h;
    int hdd, fdd, ikbd;         // just distinct addresses as core keys

    ConnectedDevices::update(&hdd, {"AAAAAAAAAAAA"});
    ConnectedDevices::update(&fdd, {"AAAAAAAAAAAA", "BBBBBBBBBBBB"});
    ConnectedDevices::update(&ikbd, {"CCCCCCCCCCCC"});
    CHECK(ConnectedDevices::getAll() == std::set<std::string>({"AAAAAAAAAAAA", "BBBBBBBBBBBB", "CCCCCCCCCCCC"}));
    CHECK(h.content() == "AAAAAAAAAAAA\nBBBBBBBBBBBB\nCCCCCCCCCCCC\n");

    ConnectedDevices::update(&fdd, {});                 // floppy connections gone - A still on HDD, B gone
    CHECK(ConnectedDevices::getAll() == std::set<std::string>({"AAAAAAAAAAAA", "CCCCCCCCCCCC"}));
    CHECK(h.content() == "AAAAAAAAAAAA\nCCCCCCCCCCCC\n");

    ConnectedDevices::update(&hdd, {});
    ConnectedDevices::update(&ikbd, {});
    CHECK(ConnectedDevices::getAll().empty());
    CHECK(h.content() == "");
}

TEST_CASE("connecteddevices: clear() empties the file")
{
    ConnectedFileHarness h;
    int hdd;

    ConnectedDevices::update(&hdd, {"AAAAAAAAAAAA"});
    CHECK(h.content() == "AAAAAAAAAAAA\n");

    ConnectedDevices::clear();
    CHECK(ConnectedDevices::getAll().empty());
    CHECK(h.content() == "");
}

TEST_CASE("connecteddevices: ChipInterface reports its connected clients with known mac")
{
    ConnectedFileHarness h;
    ChipInterface *ci = new ChipInterface(LOGFILE_HDD, NET_ATN_HANS_ID, SYNC_TAG_HDD, 0);

    uint8_t macA[6] = {0x02, 0, 0, 0, 0, 0x0a};
    ClientInfo *a = ci->clientGetByIndex(0);
    a->fdClient = 100;                      // fake fd - only checked for FD_EMPTY here
    memcpy(a->mac, macA, 6);

    ClientInfo *unknown = ci->clientGetByIndex(1);
    unknown->fdClient = 101;                // connected, mac not known yet - not reported

    ci->reportConnectedDevices();
    CHECK(ConnectedDevices::getAll() == std::set<std::string>({"02000000000A"}));

    a->fdClient = FD_EMPTY;                 // closed e.g. on send failure
    fd_set readfds;
    FD_ZERO(&readfds);
    ci->setAllClientFds(&readfds);          // core main loop reports before select()
    CHECK(ConnectedDevices::getAll().empty());

    a->fdClient = 100;
    ci->reportConnectedDevices();
    CHECK(ConnectedDevices::getAll().size() == 1);

    a->fdClient = FD_EMPTY;                 // don't let anything touch the fake fds
    unknown->fdClient = FD_EMPTY;
    delete ci;                              // core gone - its devices too
    CHECK(ConnectedDevices::getAll().empty());
}
