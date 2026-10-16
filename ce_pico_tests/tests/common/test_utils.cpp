// utils.cpp - byte order helpers, ATN header, command timeout, settings in EEPROM, debug output

#include "test_helpers.h"

TEST_SUITE("utils") {

TEST_CASE("getWord / getDword / get24bits read big-endian") {
    uint8_t b[4] = { 0x12, 0x34, 0x56, 0x78 };

    CHECK(getWord(b)   == 0x1234);
    CHECK(get24bits(b) == 0x123456);
    CHECK(getDword(b)  == 0x12345678);

    uint8_t hi[4] = { 0xff, 0xfe, 0xfd, 0xfc };     // no sign extension
    CHECK(getWord(hi)   == 0xfffe);
    CHECK(get24bits(hi) == 0xfffefd);
    CHECK(getDword(hi)  == 0xfffefdfcu);
}

TEST_CASE("storeWord / storeDword / store24bits write big-endian and touch only their bytes") {
    uint8_t b[6];

    memset(b, 0xaa, sizeof(b));
    storeWord(b + 1, 0xbeef);
    CHECK(b[0] == 0xaa); CHECK(b[1] == 0xbe); CHECK(b[2] == 0xef); CHECK(b[3] == 0xaa);

    memset(b, 0xaa, sizeof(b));
    store24bits(b + 1, 0x00c0ffee);
    CHECK(b[0] == 0xaa); CHECK(b[1] == 0xc0); CHECK(b[2] == 0xff); CHECK(b[3] == 0xee); CHECK(b[4] == 0xaa);

    memset(b, 0xaa, sizeof(b));
    storeDword(b + 1, 0xdeadbeef);
    CHECK(b[0] == 0xaa); CHECK(b[1] == 0xde); CHECK(b[2] == 0xad); CHECK(b[3] == 0xbe); CHECK(b[4] == 0xef); CHECK(b[5] == 0xaa);
}

TEST_CASE("store / get round trip") {
    uint8_t b[4];
    for (uint32_t v : { 0u, 1u, 0x7fu, 0x80u, 0xffffu, 0x10000u, 0x7fffffffu, 0x80000000u, 0xffffffffu }) {
        CAPTURE(v);
        storeDword(b, v);  CHECK(getDword(b)  == v);
        store24bits(b, v); CHECK(get24bits(b) == (v & 0xffffff));
        storeWord(b, v);   CHECK(getWord(b)   == (v & 0xffff));
    }
}

TEST_CASE("storeHeader writes sync tag, ATN code and length") {
    uint8_t b[TX_HEADER_SIZE + 1];
    memset(b, 0x55, sizeof(b));

    storeHeader(b, ATN_ACSI_COMMAND, 0x01020304);

    const uint8_t expected[TX_HEADER_SIZE] = { 0xc0, 0x50, 0xd1, 0xc5, 0x00, ATN_ACSI_COMMAND, 0x01, 0x02, 0x03, 0x04 };
    CHECK(memcmp(b, expected, TX_HEADER_SIZE) == 0);
    CHECK(b[TX_HEADER_SIZE] == 0x55);
    CHECK(getDword(b) == SYNC_TAG_HDD);     // the same tag the device expects from the host
}

TEST_CASE_FIXTURE(FirmwareFixture, "timeoutStart sets hasTimedOut only after the period elapsed") {
    timeoutStart(500);
    CHECK(fakeLastAlarmMs == 500);
    CHECK_FALSE(hasTimedOut);

    fakeAdvanceMillis(499);
    CHECK_FALSE(hasTimedOut);

    fakeAdvanceMillis(1);
    CHECK(hasTimedOut);
    CHECK_FALSE(timerRunning);
}

TEST_CASE_FIXTURE(FirmwareFixture, "timeoutStart default is the short timeout") {
    timeoutStart();
    CHECK(fakeLastAlarmMs == CMD_TIMEOUT_SHORT);
}

TEST_CASE_FIXTURE(FirmwareFixture, "timeoutStart while running restarts the timer instead of adding another one") {
    timeoutStart(500);
    fakeAdvanceMillis(400);
    timeoutStart(500);
    CHECK(fakeActiveAlarms == 1);

    fakeAdvanceMillis(400);         // 800 ms after the first start - the first timer must not fire
    CHECK_FALSE(hasTimedOut);

    fakeAdvanceMillis(100);
    CHECK(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "timeoutClear cancels the timer and clears the flag") {
    timeoutStart(100);
    timeoutClear();
    CHECK(fakeActiveAlarms == 0);

    fakeAdvanceMillis(1000);
    CHECK_FALSE(hasTimedOut);

    timeoutStart(100);
    fakeAdvanceMillis(100);
    REQUIRE(hasTimedOut);
    timeoutClear();
    CHECK_FALSE(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "alarm firing before add_alarm_in_ms returns is not lost") {
    fakeAlarmFireDuringAdd = true;      // core stalled between adding the alarm and the code after it

    timeoutStart(100);

    CHECK(hasTimedOut);
    CHECK_FALSE(timerRunning);
}

TEST_CASE_FIXTURE(FirmwareFixture, "no free alarm slot - times out right away instead of never") {
    fakeAlarmAddFails  = true;
    fakeAlarmAddResult = -1;

    timeoutStart(100);

    CHECK(hasTimedOut);
    CHECK_FALSE(timerRunning);
    timeoutClear();                     // nothing to cancel, must not break
    CHECK_FALSE(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "alarm time already passed - times out right away") {
    fakeAlarmAddFails  = true;
    fakeAlarmAddResult = 0;             // the callback won't be called in this case

    timeoutStart(1);

    CHECK(hasTimedOut);
    CHECK_FALSE(timerRunning);
}

TEST_CASE_FIXTURE(FirmwareFixture, "late callback of a cancelled alarm doesn't time out the new period") {
    timeoutStart(100);
    alarm_callback_t oldCallback = fakeLastAlarmCallback;
    void *oldUserData = fakeLastAlarmUserData;

    timeoutStart(1000);                 // restart, the old alarm is cancelled...
    oldCallback(1, oldUserData);        // ...but its callback was already running on the other core

    CHECK_FALSE(hasTimedOut);
    CHECK(timerRunning);

    fakeAdvanceMillis(1000);            // the new one still works
    CHECK(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "late callback after timeoutClear is ignored") {
    timeoutStart(100);
    alarm_callback_t oldCallback = fakeLastAlarmCallback;
    void *oldUserData = fakeLastAlarmUserData;

    timeoutClear();
    oldCallback(1, oldUserData);

    CHECK_FALSE(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "cmdTimeoutChangeLength restarts with the new period") {
    timeoutStart(100);
    cmdTimeoutChangeLength(2000);
    CHECK(fakeActiveAlarms == 1);
    CHECK(fakeLastAlarmMs == 2000);

    fakeAdvanceMillis(1999);
    CHECK_FALSE(hasTimedOut);
    fakeAdvanceMillis(1);
    CHECK(hasTimedOut);
}

TEST_CASE_FIXTURE(FirmwareFixture, "longTimeout_basedOnSectorCount gives 3 s per started MB, max 30 s") {
    struct { uint16_t sectors; uint32_t ms; } cases[] = {
        {     0,  3000 },
        {     1,  3000 },
        {  2047,  3000 },       // 2047 * 512 B < 1 MB
        {  2048,  6000 },
        {  4095,  6000 },
        {  4096,  9000 },
        { 18431, 27000 },       // 8.99 MB -> 9 MB
        { 18432, 30000 },       // 10 MB -> 30 s
        { 65535, 30000 },       // capped
    };

    for (auto &c : cases) {
        CAPTURE(c.sectors);
        longTimeout_basedOnSectorCount(c.sectors);
        CHECK(fakeLastAlarmMs == c.ms);
        CHECK(fakeActiveAlarms == 1);
    }
}

TEST_CASE_FIXTURE(FirmwareFixture, "TSettings layout in EEPROM is stable") {
    CHECK(sizeof(TSettings) == 12);         // changing it would make the stored settings of existing devices invalid
    CHECK(offsetof(TSettings, magic) == 0);
    CHECK(offsetof(TSettings, mac) == 4);
    CHECK(offsetof(TSettings, enabledIDs) == 10);
    CHECK(offsetof(TSettings, ikbdEnabled) == 11);
}

TEST_CASE_FIXTURE(FirmwareFixture, "loadSettings on erased EEPROM stores defaults") {
    memset(&settings, 0, sizeof(settings));

    loadSettings();

    CHECK((uint32_t) settings.magic == SETTINGS_MAGIC);
    CHECK(settings.enabledIDs == 0x01);                 // only ID 0
    CHECK((settings.mac[0] & 0x01) == 0);               // unicast
    CHECK((settings.mac[0] & 0x02) == 0x02);            // locally administered
    CHECK(EEPROM.commitCount == 1);
    CHECK(fakeLockoutCount == 1);                       // other core locked out while writing flash

    TSettings stored;
    memcpy(&stored, EEPROM.bytes, sizeof(stored));
    CHECK(memcmp(&stored, &settings, sizeof(settings)) == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "loadSettings on erased EEPROM makes the MAC from the board ID") {
    uint8_t expected[6];
    makeMacFromBoardId(expected);

    loadSettings();
    CHECK(memcmp(settings.mac, expected, 6) == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "makeMacFromBoardId - same board same MAC, different boards different MACs") {
    uint8_t a[6], b[6], c[6];

    makeMacFromBoardId(a);
    makeMacFromBoardId(b);
    CHECK(memcmp(a, b, 6) == 0);        // stable - not random

    fakeBoardId[7] ^= 0x01;             // boards differing in a single bit of the ID
    makeMacFromBoardId(c);
    CHECK(memcmp(a, c, 6) != 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "makeMacFromBoardId - every ID byte affects the MAC, always unicast + locally administered") {
    uint8_t base[6];
    makeMacFromBoardId(base);

    for (int byte = 0; byte < 8; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            CAPTURE(byte); CAPTURE(bit);

            fakeBoardId[byte] ^= (1 << bit);
            uint8_t mac[6];
            makeMacFromBoardId(mac);
            fakeBoardId[byte] ^= (1 << bit);

            CHECK((mac[0] & 0x03) == 0x02);
            bool lostBit = (byte % 6 == 0) && bit < 2;  // 2 lowest bits of mac[0] are fixed
            CHECK((memcmp(mac, base, 6) != 0) == !lostBit);
        }
    }
}

TEST_CASE_FIXTURE(FirmwareFixture, "loadSettings keeps valid stored settings and does not write") {
    TSettings stored = {};
    stored.magic = SETTINGS_MAGIC;
    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(stored.mac, mac, 6);
    stored.enabledIDs  = 0x81;
    stored.ikbdEnabled = 1;
    memcpy(EEPROM.bytes, &stored, sizeof(stored));

    memset(&settings, 0, sizeof(settings));
    loadSettings();

    CHECK(memcmp(&settings, &stored, sizeof(stored)) == 0);
    CHECK(EEPROM.commitCount == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "saveSettings writes all settings bytes with magic and commits") {
    settings.magic       = 0;           // saveSettings sets the magic itself
    settings.enabledIDs  = 0x24;
    settings.ikbdEnabled = 1;

    saveSettings();

    TSettings stored;
    memcpy(&stored, EEPROM.bytes, sizeof(stored));
    CHECK((uint32_t) stored.magic == SETTINGS_MAGIC);
    CHECK(stored.enabledIDs == 0x24);
    CHECK(stored.ikbdEnabled == 1);
    CHECK(EEPROM.commitCount == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "debug formats and turns \\n into \\n\\r") {
    debug("val %d %s\nnext\n", 42, "x");
#ifdef LOG_LED
    CHECK(fakeDebugOut == "val 42 x\n\rnext\n\r");
#endif
}

TEST_CASE_FIXTURE(FirmwareFixture, "debug output longer than the buffer is truncated") {
    std::string longText(3000, 'x');
    debug("%s\n", longText.c_str());
#ifdef LOG_LED
    CHECK(fakeDebugOut == std::string(1023, 'x'));     // 1023 chars + terminator, the \n didn't fit
#endif
}

TEST_CASE_FIXTURE(FirmwareFixture, "debug output with many newlines fits the converted buffer") {
    std::string newlines(1500, '\n');
    debug("%s", newlines.c_str());
#ifdef LOG_LED
    std::string expected;
    for (int i = 0; i < 1023; i++) expected += "\n\r";
    CHECK(fakeDebugOut == expected);
#endif
}

TEST_CASE_FIXTURE(FirmwareFixture, "debug from IRQ handler is dropped, so it can't deadlock on the mutex") {
    fakeException = 16 + 7;             // some PIO IRQ
    mutex_enter_blocking(&debugMutex);  // e.g. interrupted code is in the middle of debug()
    debug("from irq\n");               // would wait forever on the real mutex
    mutex_exit(&debugMutex);
    CHECK(debugMutex.locked == 0);
    CHECK(fakeDebugOut.empty());

    fakeException = 0;
    debug("from thread\n");
#ifdef LOG_LED
    CHECK(fakeDebugOut == "from thread\n\r");
#endif
}

TEST_CASE_FIXTURE(FirmwareFixture, "debug leaves the mutex unlocked") {
    debug("x %d\n", 1);
    CHECK(debugMutex.locked == 0);
}

#ifdef LOG_LED
TEST_CASE_FIXTURE(FirmwareFixture, "debug on core 1 is queued until debugFromQueue on core 0") {
    fakeCoreNum = 1;
    debug("c1\n");
    CHECK(fakeDebugOut.empty());

    fakeCoreNum = 0;
    debugFromQueue();
    CHECK(fakeDebugOut == "c1\n\r");
}
#endif

}
