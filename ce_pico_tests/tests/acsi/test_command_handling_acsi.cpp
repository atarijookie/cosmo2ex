// ce_pico_hdd/command_handling.cpp, ACSI variant - receiving ACSI / ICD commands, data transfers and status towards the Atari

#include "test_helpers.h"
#include <bridge_fake.h>         // tests/hdd/bridge_fake.h
#include "bridge.h"

namespace {

struct AcsiFixture : FirmwareFixture {
    AcsiFixture() {
        fakeBridge = FakeBridge();
        hwVariant = VARIANT_ACSI;     // merged firmware - run the ACSI code paths
    }
};

void fromAtari(const std::vector<uint8_t> &bytes)
{
    fakeBridge.fromAtari.insert(fakeBridge.fromAtari.end(), bytes.begin(), bytes.end());
}

}

TEST_SUITE("acsi command handling") {

TEST_CASE_FIXTURE(AcsiFixture, "idIsEnabled follows the enabledIDs bits") {
    settings.enabledIDs = 0x85;     // IDs 0, 2, 7

    CHECK(idIsEnabled(0));
    CHECK_FALSE(idIsEnabled(1));
    CHECK(idIsEnabled(2));
    CHECK_FALSE(idIsEnabled(6));
    CHECK(idIsEnabled(7));
    CHECK_FALSE(idIsEnabled(8));
    CHECK_FALSE(idIsEnabled(0xff));
}

TEST_CASE_FIXTURE(AcsiFixture, "ACSI command length - 6 bytes, ICD (0x1f) by command group") {
    struct { uint8_t c0, c1, len; } cases[] = {
        { 0x08, 0x00,  6 },     // READ(6)
        { 0x0a, 0xff,  6 },     // 2nd byte ignored for non ICD commands
        { 0x3f, 0x00,  7 },     // ICD, group 0
        { 0x1f, 0x00,  7 },
        { 0x1f, 0x28, 11 },     // group 1 - READ(10)
        { 0x1f, 0x2a, 11 },     // group 1 - WRITE(10)
        { 0x1f, 0x5a, 11 },     // group 2
        { 0x1f, 0xa8, 13 },     // group 5 - READ(12)
        { 0x1f, 0x88,  7 },     // group 4 - 16 byte CDBs don't fit, treated as default
        { 0x1f, 0x60,  7 },     // group 3
        { 0x1f, 0xc0,  7 },     // group 6
        { 0x1f, 0xe0,  7 },     // group 7
    };

    for (auto &c : cases) {
        CAPTURE(c.c0); CAPTURE(c.c1);
        cmd[0] = c.c0;
        cmd[1] = c.c1;
        getCmdLengthFromCmdBytesAcsi();
        CHECK(cmdLen == c.len);
    }
}

TEST_CASE_FIXTURE(AcsiFixture, "SCSI command length by command group") {
    struct { uint8_t c0, len; } cases[] = {
        { 0x00, 6 }, { 0x08, 6 }, { 0x28, 10 }, { 0x5a, 10 }, { 0x60, 6 }, { 0x88, 16 }, { 0xa8, 12 }, { 0xc0, 6 }, { 0xe0, 6 },
    };

    for (auto &c : cases) {
        CAPTURE(c.c0);
        getCmdLengthFromCmdBytesScsi(c.c0);
        CHECK(cmdLen == c.len);
    }
}

TEST_CASE_FIXTURE(AcsiFixture, "6 byte command for an enabled ID") {
    settings.enabledIDs = 0x01;
    fromAtari({ 0x08, 0x00, 0x00, 0x10, 0x02, 0x00 });     // ID 0, READ(6), sector 0x10, 2 sectors

    REQUIRE(onGetCommandAcsi() == 1);
    CHECK(cmdLen == 6);
    CHECK(std::vector<uint8_t>(cmd, cmd + 6) == std::vector<uint8_t>{ 0x08, 0x00, 0x00, 0x10, 0x02, 0x00 });
    CHECK(fakeBridge.fromAtari.empty());
    CHECK(fakeBridge.resetCount == 0);
}

TEST_CASE_FIXTURE(AcsiFixture, "command for ID 5 when only ID 5 is enabled") {
    settings.enabledIDs = 0x20;
    fromAtari({ 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00 });     // ID 5, TEST UNIT READY

    REQUIRE(onGetCommandAcsi() == 1);
    CHECK(cmd[0] == 0xa0);
}

TEST_CASE_FIXTURE(AcsiFixture, "command for a disabled ID is ignored after the 1st byte") {
    settings.enabledIDs = 0x01;
    fromAtari({ 0x28, 0x00, 0x00, 0x00, 0x01, 0x00 });     // ID 1

    CHECK(onGetCommandAcsi() == 0);
    CHECK(fakeBridge.resetCount == 1);
    CHECK(fakeBridge.fromAtari.size() == 5);                // the rest is not for us, not read
}

TEST_CASE_FIXTURE(AcsiFixture, "ICD READ(10) is read as 11 bytes") {
    std::vector<uint8_t> icd = { 0x1f, 0x28, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0x08, 0x00 };
    fromAtari(icd);
    fromAtari({ 0x99 });                                    // must not be read

    REQUIRE(onGetCommandAcsi() == 1);
    CHECK(cmdLen == 11);
    CHECK(std::vector<uint8_t>(cmd, cmd + 11) == icd);
    CHECK(fakeBridge.fromAtari.size() == 1);
}

TEST_CASE_FIXTURE(AcsiFixture, "ICD READ(12) is read as 13 bytes") {
    std::vector<uint8_t> icd = { 0x1f, 0xa8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    fromAtari(icd);

    REQUIRE(onGetCommandAcsi() == 1);
    CHECK(cmdLen == 13);
    CHECK(std::vector<uint8_t>(cmd, cmd + 13) == icd);
}

TEST_CASE_FIXTURE(AcsiFixture, "timeout in the middle of the command fails and resets the bridge") {
    fromAtari({ 0x08, 0x00, 0x00 });

    CHECK(onGetCommandAcsi() == 0);
    CHECK(fakeBridge.resetCount == 1);
}

TEST_CASE_FIXTURE(AcsiFixture, "onGetCommand passes the command to core 0 and starts the timeout") {
    std::vector<uint8_t> icd = { 0x1f, 0x28, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x01, 0x00 };
    fromAtari(icd);

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(fakeActiveAlarms == 1);
    CHECK(fakeLastAlarmMs == CMD_TIMEOUT_SHORT);

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_GET_COMMAND);
    CHECK(msgs[0].length == 11);
    CHECK(std::vector<uint8_t>(msgs[0].data.begin(), msgs[0].data.begin() + 11) == icd);
    CHECK(drainIpc(1).empty());
}

TEST_CASE_FIXTURE(AcsiFixture, "onGetCommand failure stays in STATE_GET_COMMAND, nothing sent") {
    settings.enabledIDs = 0x01;
    fromAtari({ 0x48 });            // ID 2, disabled

    timeoutStart(1000);
    CHECK(onGetCommand() == STATE_GET_COMMAND);
    CHECK(fakeActiveAlarms == 0);   // timeout cleared
    CHECK(drainIpc(0).empty());
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataRead sends all bytes with DMA") {
    std::vector<uint8_t> data = { 1, 2, 3, 4, 5 };

    CHECK(onDataRead(data.size(), data.data()));
    CHECK(fakeBridge.dmaToAtari == data);
    REQUIRE_FALSE(fakeBridge.modes.empty());
    CHECK(fakeBridge.modes.back() == MODE_DMA_READ);
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataRead fails on DMA timeout") {
    std::vector<uint8_t> data(10, 0x11);
    fakeBridge.dmaReadFailAfter = 4;

    CHECK_FALSE(onDataRead(data.size(), data.data()));
    CHECK(fakeBridge.dmaToAtari.size() == 4);
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataRead fails when waiting for the end of DMA times out") {
    uint8_t b = 0;
    fakeBridge.waitForEndFails = true;
    CHECK_FALSE(onDataRead(1, &b));
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataWrite announces the count, then forwards the data in 512 B chunks") {
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); i++) data[i] = i ^ 0x5a;
    fromAtari(data);

    CHECK(onDataWrite(data.size()));
    CHECK(fakeBridge.dmaWriteCount == 1000);
    CHECK(fakeBridge.modes.back() == MODE_DMA_WRITE);

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0].command == STATE_SEND_WRITE_MORE_DATA);
    CHECK(msgs[0].length == 1000);
    CHECK(msgs[1].command == STATE_DATA_WRITE);
    CHECK(msgs[1].length == 512);
    CHECK(msgs[2].command == STATE_DATA_WRITE);
    CHECK(msgs[2].length == 488);

    std::vector<uint8_t> received = msgs[1].data;
    received.insert(received.end(), msgs[2].data.begin(), msgs[2].data.end());
    CHECK(received == data);
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataWrite of 0 bytes sends just the header, no DMA") {
    fakeBridge.dmaWriteCount = 12345;

    CHECK(onDataWrite(0));

    CHECK(fakeBridge.dmaWriteCount == 12345);       // DMA_write_startWithCount() not called
    CHECK(fakeBridge.modes.empty());                // bus mode not touched

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);                      // host waits for ATN_WRITE_MORE_DATA even for 0 bytes
    CHECK(msgs[0].command == STATE_SEND_WRITE_MORE_DATA);
    CHECK(msgs[0].length == 0);
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataWrite fails on DMA timeout") {
    fromAtari(std::vector<uint8_t>(100, 0x22));
    CHECK_FALSE(onDataWrite(512));
}

TEST_CASE_FIXTURE(AcsiFixture, "onDataWrite timeout does not lose an IPC buffer") {
    fromAtari(std::vector<uint8_t>(100, 0x22));
    REQUIRE_FALSE(onDataWrite(512));
    drainIpc(0);                    // core 0 handles and frees what was queued

    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        CAPTURE(i);
        CHECK(buffersForCore0[i].free);
    }
}

TEST_CASE_FIXTURE(AcsiFixture, "onReadStatus sends the status byte") {
    onReadStatus(0x02);
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ 0x02 });
}

}
