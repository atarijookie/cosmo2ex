// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of DESKTOP.INF / NEWDESK.INF creation for the config drive

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "hdd/translated/desktopcreator.h"

static std::vector<char> readWholeFile(const std::string& path)
{
    std::vector<char> data;
    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE(f != NULL);

    char bfr[4096];
    size_t n;
    while((n = fread(bfr, 1, sizeof(bfr), f)) > 0) {
        data.insert(data.end(), bfr, bfr + n);
    }
    fclose(f);
    return data;
}

TEST_CASE("DesktopCreator::createToFile - the file has exactly the bytes of the buffer (no new line conversion)")
{
    char tmpl[] = "/tmp/ce_test_desktop_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != NULL);
    std::string oldConfigDrive = CONFIG_DRIVE_PATH;
    CONFIG_DRIVE_PATH = tmpl;

    DesktopConfig dc;
    dc.currentResolution = 1;
    dc.settingsResolution = 1;
    dc.drivesAll = (1 << 0) | (1 << 2) | (1 << 3);              // A, C, D
    dc.translatedDrives = (1 << 3);
    dc.configDrive = 3;

    // the lines end with \r\n already - on Cygwin a file opened in text mode ("wt") turned them into \r\r\n
    struct { uint16_t tos; const char* file; } cases[] = { {TOSVER104, "DESKTOP.INF"}, {TOSVER206, "NEWDESK.INF"} };

    for(auto& c : cases) {
        CAPTURE(c.file);
        dc.tosVersion = c.tos;

        char bfr[10240];
        int len = DesktopCreator::createToBuffer(bfr, sizeof(bfr), &dc);
        REQUIRE(len > 0);

        DesktopCreator::createToFile(&dc);

        std::vector<char> onDisk = readWholeFile(std::string(tmpl) + "/" + c.file);
        CHECK(onDisk == std::vector<char>(bfr, bfr + len));
        CHECK(std::string(onDisk.begin(), onDisk.end()).find("\r\r\n") == std::string::npos);
    }

    CONFIG_DRIVE_PATH = oldConfigDrive;
    system((std::string("rm -rf '") + tmpl + "'").c_str());
}
