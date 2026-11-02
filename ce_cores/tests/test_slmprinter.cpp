/*
    Tests of SlmPrinter - SLM605 / SLM804 laser printer emulation (hdd/slm/)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <deque>
#include <map>

#include "doctest.h"

#include "misc/global.h"
#include "misc/utils.h"
#include "misc/settings.h"
#include "hdd/acsidatatrans.h"
#include "hdd/slm/slmprinter.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

// records what the printer sends to the device, gives it what ST would send
class FakeSlmTrans : public AcsiDataTrans
{
public:
    std::vector<uint8_t>                statuses;       // sendStatusToHans()
    std::vector<std::vector<uint8_t>>   pioReads;       // pioRead() - status + extended status bytes
    std::deque<std::vector<uint8_t>>    pioWrites;      // what pioWrite() returns, one item per call
    int  pagesRequested = 0;
    bool pagesFail      = false;
    uint32_t lastTotal = 0, lastBurst = 0, lastFirstBurst = 0, lastPeriod = 0;
    uint8_t  lastTimeoutStatus = 0;

    bool sendStatusToHans(uint8_t statusByte) override {
        statuses.push_back(statusByte);
        return true;
    }

    bool pioRead(uint8_t *data, uint32_t cnt) override {
        pioReads.push_back(std::vector<uint8_t>(data, data + cnt));
        return true;
    }

    bool pioWrite(uint8_t *data, uint32_t cnt) override {
        REQUIRE(!pioWrites.empty());
        std::vector<uint8_t> v = pioWrites.front();
        pioWrites.pop_front();
        REQUIRE(v.size() == cnt);
        memcpy(data, v.data(), cnt);
        return true;
    }

    bool recvDataPaced(uint8_t *data, uint32_t cnt, uint16_t firstBurst, uint16_t burst, uint32_t burstPeriodUs,
                       uint16_t startDelayMs, uint8_t timeoutStatus) override {
        pagesRequested++;
        lastTotal = cnt; lastFirstBurst = firstBurst; lastBurst = burst; lastPeriod = burstPeriodUs; lastTimeoutStatus = timeoutStatus;

        if(pagesFail) {
            return false;
        }

        for(uint32_t i = 0; i < cnt; i++) {         // left half of each line black
            data[i] = ((i % burst) < burst / 2) ? 0xff : 0x00;
        }
        return true;
    }
};

static void cmd6(uint8_t* cmd, uint8_t id, uint8_t opcode, uint8_t b4 = 0, uint8_t b5 = 0)
{
    memset(cmd, 0, ACSI_CMD_SIZE);
    cmd[0] = (id << 5) | opcode;
    cmd[4] = b4;
    cmd[5] = b5;
}

// temp dirs for settings (paper size) and printed pages - printer reads the paper size setting on each command
struct SlmDirs {
    std::string dir, settingsDir;

    SlmDirs() {
        char tmpl[] = "/tmp/ce_test_slm_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        dotEnv["PRINTOUTS_DIR"] = dir;

        char tmplSet[] = "/tmp/ce_test_slmset_XXXXXX";
        REQUIRE(mkdtemp(tmplSet) != NULL);
        settingsDir = tmplSet;
        dotEnv["SETTINGS_DIR"] = settingsDir;
    }

    ~SlmDirs() {
        dotEnv.erase("PRINTOUTS_DIR");
        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + dir + "' '" + settingsDir + "'";
        system(cmd.c_str());
    }

    void setPaper(const char* key) {
        Settings s;
        s.setString(SLM_PAPER_SETTING, key);
    }

    int pngCount(void) {
        std::string cmd = "ls '" + dir + "' 2>/dev/null | grep -c '\\.png$'";
        FILE* f = popen(cmd.c_str(), "r");
        int cnt = 0;
        if(f) { fscanf(f, "%d", &cnt); pclose(f); }
        return cnt;
    }

    bool waitForPngs(int count) {           // pages are saved in background threads
        for(int i = 0; i < 300 && pngCount() < count; i++) {
            Utils::sleepMs(10);
        }
        return pngCount() == count;
    }
};

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - INQUIRY without Extended Status bit fails, with it returns identification")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    cmd6(cmd, 7, SLM_OP_INQUIRY);
    CHECK_FALSE(slm.processCommand(cmd, next));
    REQUIRE(dt.statuses.size() == 1);
    CHECK(dt.statuses[0] == SLM_ST_INVALID_OPCODE);

    cmd6(cmd, 7, SLM_OP_INQUIRY, 0, 0x80);
    slm.processCommand(cmd, next);
    REQUIRE(dt.pioReads.size() == 1);
    const std::vector<uint8_t>& r = dt.pioReads[0];
    CHECK(r[0] == 0x00);                                    // null status byte first
    CHECK(r[1] == 0x02);                                    // printer
    CHECK(r[5] == r.size() - 6);                            // string length
    CHECK(std::string(r.begin() + 6, r.end()).rfind("PAGE PRINTER:", 0) == 0);     // DIAB630 checks this prefix
    CHECK(r.size() <= PIO_READ_MAX_LEN);
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - device number other than 0 is invalid, STOP PRINT without printing has no status")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    cmd6(cmd, 7, SLM_OP_REQUEST_SENSE);
    cmd[1] = 2 << 5;
    slm.processCommand(cmd, next);
    REQUIRE(dt.statuses.size() == 1);
    CHECK(dt.statuses[0] == ((2 << 5) | SLM_ST_INVALID_DEVICE));

    cmd6(cmd, 7, SLM_OP_STOP_PRINT);
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.size() == 1);                         // no new status
    REQUIRE(dt.pioReads.size() == 1);
    CHECK(dt.pioReads[0].empty());                          // just end of command on the device

    cmd6(cmd, 7, 0x08);                                     // e.g. disk READ - not a printer command
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.back() == SLM_ST_INVALID_OPCODE);
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - MODE SENSE returns default, absolute and partial lists")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    cmd6(cmd, 7, SLM_OP_MODE_SENSE);                        // length 0 = whole list
    slm.processCommand(cmd, next);
    REQUIRE(dt.pioReads.size() == 1);
    std::vector<uint8_t> r = dt.pioReads[0];
    REQUIRE(r.size() == 2 + SLM_PARAM_LIST_LEN);
    CHECK(r[0] == 0x00);                                    // status
    CHECK(r[1] == SLM_PARAM_LIST_LEN);
    CHECK(Utils::getWord(&r[2]) == 3180);                   // letter block height
    CHECK(Utils::getWord(&r[4]) == 2400);                   // block width
    CHECK(Utils::getWord(&r[11]) == 300);                   // vertical resolution
    CHECK(Utils::getWord(&r[16]) == SLM_SCAN_TIME_US);

    cmd6(cmd, 7, SLM_OP_MODE_SENSE, 44, 0x80);              // more than we have, absolute
    slm.processCommand(cmd, next);
    r = dt.pioReads[1];
    CHECK(r[1] == SLM_PARAM_LIST_LEN);
    CHECK(Utils::getWord(&r[2]) == 4080);                   // legal is the maximum

    cmd6(cmd, 7, SLM_OP_MODE_SENSE, 2);                     // just block height
    slm.processCommand(cmd, next);
    r = dt.pioReads[2];
    REQUIRE(r.size() == 4);
    CHECK(r[1] == 2);
    CHECK(Utils::getWord(&r[2]) == 3180);
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - MODE SELECT changes parameters, rejects invalid ones, Reset Default restores them")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    uint8_t list[1 + SLM_PARAM_LIST_LEN];
    slm.buildParamList(list, false);
    list[9] |= 0x01;                                        // manual feed - DIAB630 toggles this
    Utils::storeWord(list + 1, 3386);                       // A4 height

    dt.pioWrites.push_back(std::vector<uint8_t>(1, SLM_PARAM_LIST_LEN));
    dt.pioWrites.push_back(std::vector<uint8_t>(list + 1, list + 1 + SLM_PARAM_LIST_LEN));
    cmd6(cmd, 7, SLM_OP_MODE_SELECT);
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.back() == SLM_ST_OK);
    CHECK(slm.getParams().paperFlags == 0x01);
    CHECK(slm.getParams().blockHeight == 3386);

    Utils::storeWord(list + 3, 4000);                       // too wide
    dt.pioWrites.push_back(std::vector<uint8_t>(1, SLM_PARAM_LIST_LEN));
    dt.pioWrites.push_back(std::vector<uint8_t>(list + 1, list + 1 + SLM_PARAM_LIST_LEN));
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.back() == SLM_ST_INVALID_PARAMS);
    CHECK(slm.getParams().blockWidth == 2400);              // not changed

    dt.pioWrites.push_back(std::vector<uint8_t>(1, 0));     // empty list - nothing changes
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.back() == SLM_ST_OK);
    CHECK(slm.getParams().blockHeight == 3386);

    cmd6(cmd, 7, SLM_OP_MODE_SELECT, 0, 0x80);              // Reset Default - no list
    slm.processCommand(cmd, next);
    CHECK(dt.statuses.back() == SLM_ST_OK);
    CHECK(slm.getParams().blockHeight == 3180);
    CHECK(slm.getParams().paperFlags == 0);
    CHECK(dt.pioWrites.empty());
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - bitmap to gray: 1 is black, bits after width ignored")
{
    const uint8_t bitmap[4] = {0x80, 0x01, 0x00, 0xff};     // 2 lines, 2 bytes each, width 12
    uint8_t gray[24];
    SlmPrinter::bitmapToGray(bitmap, 2, 12, 2, gray);

    CHECK(gray[0] == 0x00);
    CHECK(gray[1] == 0xff);
    CHECK(gray[7] == 0xff);
    CHECK(gray[8] == 0xff);                                 // 2nd byte, bit 7
    CHECK(gray[11] == 0xff);                                // bit 0 of 2nd byte would be x = 15, outside
    CHECK(gray[12] == 0xff);
    CHECK(gray[12 + 8] == 0x00);
    CHECK(gray[12 + 11] == 0x00);
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - page file name")
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 126; tm.tm_mon = 10; tm.tm_mday = 1; tm.tm_hour = 9; tm.tm_min = 5; tm.tm_sec = 7; tm.tm_isdst = -1;
    time_t t = mktime(&tm);

    CHECK(SlmPrinter::pageFileName(t, 1) == "2026-11-01_09-05-07.png");
    CHECK(SlmPrinter::pageFileName(t, 3) == "2026-11-01_09-05-07_3.png");
}

// small page, so PNG writing in tests is fast
static void setSmallPage(SlmPrinter& slm, FakeSlmTrans& dt)
{
    uint8_t list[1 + SLM_PARAM_LIST_LEN];
    slm.buildParamList(list, false);
    Utils::storeWord(list + 1, 10);         // 10 lines
    Utils::storeWord(list + 3, 64);         // 64 pixels = 8 bytes per line

    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];
    dt.pioWrites.push_back(std::vector<uint8_t>(1, SLM_PARAM_LIST_LEN));
    dt.pioWrites.push_back(std::vector<uint8_t>(list + 1, list + 1 + SLM_PARAM_LIST_LEN));
    cmd6(cmd, 7, SLM_OP_MODE_SELECT);
    slm.processCommand(cmd, next);
    REQUIRE(dt.statuses.back() == SLM_ST_OK);
    dt.statuses.clear();
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - PRINT one page: paced transfer params, status, PNG saved")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    setSmallPage(slm, dt);

    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];
    cmd6(cmd, 7, SLM_OP_PRINT, 0);                          // 0 = one page
    CHECK_FALSE(slm.processCommand(cmd, next));

    CHECK(dt.pagesRequested == 1);
    CHECK(dt.lastTotal == 80);
    CHECK(dt.lastBurst == 8);
    CHECK(dt.lastFirstBurst == 10);                         // + two-byte FIFO
    CHECK(dt.lastPeriod == SLM_SCAN_TIME_US);
    CHECK(dt.lastTimeoutStatus == SLM_ST_SYSTEM_TIMEOUT);
    REQUIRE(dt.statuses.size() == 1);
    CHECK(dt.statuses[0] == SLM_ST_OK);
    CHECK(waitForPngs(1));

    // page count in MODE SENSE
    cmd6(cmd, 7, SLM_OP_MODE_SENSE);
    slm.processCommand(cmd, next);
    CHECK(Utils::getWord(&dt.pioReads.back()[18]) == 1);
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - PRINT fails when page data doesn't come - no status, device sent it")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    setSmallPage(slm, dt);
    dt.pagesFail = true;

    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];
    cmd6(cmd, 7, SLM_OP_PRINT, 3);
    CHECK_FALSE(slm.processCommand(cmd, next));
    CHECK(dt.pagesRequested == 1);
    CHECK(dt.statuses.empty());
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - multiple pages, STOP PRINT, other command")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    setSmallPage(slm, dt);

    std::deque<std::vector<uint8_t>> incoming;      // commands which come in the STOP PRINT window, empty item = nothing came
    int waits = 0;
    slm.setWaitForCommand([&](uint32_t timeoutMs, uint8_t* cmd) {
        waits++;
        CHECK(timeoutMs == SLM_STOP_PRINT_WINDOW_MS);
        if(incoming.empty() || incoming.front().empty()) {
            if(!incoming.empty()) incoming.pop_front();
            return false;
        }
        memcpy(cmd, incoming.front().data(), ACSI_CMD_SIZE);
        incoming.pop_front();
        return true;
    });

    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    SUBCASE("3 pages, nothing in the window") {
        cmd6(cmd, 7, SLM_OP_PRINT, 3);
        CHECK_FALSE(slm.processCommand(cmd, next));
        CHECK(dt.pagesRequested == 3);
        CHECK(dt.statuses.size() == 3);
        CHECK(waits == 2);                          // no window after the last page
        CHECK(waitForPngs(3));
    }

    SUBCASE("indefinite, STOP PRINT after 2nd page - 3rd page is the last one") {
        uint8_t stop[ACSI_CMD_SIZE];
        cmd6(stop, 7, SLM_OP_STOP_PRINT);
        incoming.push_back(std::vector<uint8_t>());
        incoming.push_back(std::vector<uint8_t>(stop, stop + ACSI_CMD_SIZE));

        cmd6(cmd, 7, SLM_OP_PRINT, SLM_PRINT_INDEFINITE);
        CHECK_FALSE(slm.processCommand(cmd, next));
        CHECK(dt.pagesRequested == 3);
        CHECK(dt.statuses.size() == 3);             // no status for STOP PRINT itself
        REQUIRE(dt.pioReads.size() == 1);
        CHECK(dt.pioReads[0].empty());              // STOP PRINT just ended on device
        CHECK(waitForPngs(3));
    }

    SUBCASE("other command in the window aborts printing and is returned") {
        uint8_t other[ACSI_CMD_SIZE];
        cmd6(other, 1, 0x08);                       // disk READ on other ID
        incoming.push_back(std::vector<uint8_t>(other, other + ACSI_CMD_SIZE));

        cmd6(cmd, 7, SLM_OP_PRINT, 5);
        CHECK(slm.processCommand(cmd, next));
        CHECK(memcmp(next, other, ACSI_CMD_SIZE) == 0);
        CHECK(dt.pagesRequested == 1);
        CHECK(waitForPngs(1));
    }
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - paper size from settings: default letter, change applies on next command")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];
    cmd6(cmd, 7, SLM_OP_MODE_SENSE);

    slm.processCommand(cmd, next);                          // no setting - letter
    CHECK(std::string(slm.getPaper()->key) == "letter");
    CHECK(Utils::getWord(&dt.pioReads.back()[2]) == 3180);
    CHECK(Utils::getWord(&dt.pioReads.back()[4]) == 2400);

    setPaper("a4");                                         // A4 put in the cassette
    slm.processCommand(cmd, next);
    CHECK(std::string(slm.getPaper()->key) == "a4");
    CHECK(Utils::getWord(&dt.pioReads.back()[2]) == 3386);
    CHECK(Utils::getWord(&dt.pioReads.back()[4]) == 2336);
    CHECK(slm.bytesPerLine() == 292);

    setPaper("b5");
    slm.processCommand(cmd, next);
    CHECK(slm.getParams().blockHeight == 2914);
    CHECK(slm.getParams().blockWidth == 2016);

    setPaper("legal");
    slm.processCommand(cmd, next);
    CHECK(slm.getParams().blockHeight == 4080);

    setPaper("nonsense");                                   // unknown value - letter
    slm.processCommand(cmd, next);
    CHECK(std::string(slm.getPaper()->key) == "letter");
}

TEST_CASE_FIXTURE(SlmDirs, "SlmPrinter - MODE SELECT size kept until paper changes, Reset Default uses current paper")
{
    FakeSlmTrans dt;
    SlmPrinter slm(&dt);
    uint8_t cmd[ACSI_CMD_SIZE], next[ACSI_CMD_SIZE];

    setPaper("a4");
    setSmallPage(slm, dt);                                  // MODE SELECT: 64 x 10
    CHECK(slm.getParams().blockHeight == 10);

    cmd6(cmd, 7, SLM_OP_REQUEST_SENSE);                     // same paper - MODE SELECT values stay
    slm.processCommand(cmd, next);
    CHECK(slm.getParams().blockHeight == 10);

    cmd6(cmd, 7, SLM_OP_MODE_SELECT, 0, 0x80);              // Reset Default - A4, not letter
    slm.processCommand(cmd, next);
    CHECK(slm.getParams().blockHeight == 3386);
    CHECK(slm.getParams().blockWidth == 2336);

    setSmallPage(slm, dt);
    setPaper("letter");                                     // paper changed - size follows it
    cmd6(cmd, 7, SLM_OP_REQUEST_SENSE);
    slm.processCommand(cmd, next);
    CHECK(slm.getParams().blockHeight == 3180);
    CHECK(slm.getParams().blockWidth == 2400);
}
