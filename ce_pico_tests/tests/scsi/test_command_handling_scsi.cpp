// ce_pico_hdd/command_handling.cpp, SCSI variant - receiving SCSI commands (converted to ACSI / ICD form for the host),
// MESSAGE OUT on ATN, data transfers and status towards the Atari

#include "test_helpers.h"
#include <bridge_fake.h>         // tests/hdd/bridge_fake.h
#include "bridge.h"

namespace {

struct ScsiFixture : FirmwareFixture {
    ScsiFixture() {
        fakeBridge = FakeBridge();
        hwVariant = VARIANT_SCSI;     // merged firmware - run the SCSI code paths
    }
};

void fromAtari(const std::vector<uint8_t> &bytes)
{
    fakeBridge.fromAtari.insert(fakeBridge.fromAtari.end(), bytes.begin(), bytes.end());
}

}

TEST_SUITE("scsi command handling") {

TEST_CASE_FIXTURE(ScsiFixture, "SCSI command length by command group") {
    struct { uint8_t c0, len; } cases[] = {
        { 0x00,  6 },   // TEST UNIT READY
        { 0x08,  6 },   // READ(6)
        { 0x1f,  6 },
        { 0x25, 10 },   // READ CAPACITY
        { 0x28, 10 },   // READ(10)
        { 0x5a, 10 },   // MODE SENSE(10), group 2
        { 0x60,  6 },   // group 3, reserved
        { 0x88, 16 },   // READ(16), group 4
        { 0xa8, 12 },   // READ(12), group 5
        { 0xc0,  6 },   // group 6, vendor
        { 0xe0,  6 },   // group 7, vendor
    };

    for (auto &c : cases) {
        CAPTURE(c.c0);
        getCmdLengthFromCmdBytesScsi(c.c0);
        CHECK(cmdLen == c.len);
    }
}

TEST_CASE_FIXTURE(ScsiFixture, "not selected - no command") {
    fakeBridge.selectedId = 0xff;
    fromAtari({ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });

    CHECK(onGetCommandScsi() == 0);
    CHECK(fakeBridge.fromAtari.size() == 6);
}

TEST_CASE_FIXTURE(ScsiFixture, "6 byte command gets the ID in the top 3 bits of byte 0, like ACSI") {
    fakeBridge.selectedId = 2;
    fromAtari({ 0x08, 0x00, 0x00, 0x10, 0x02, 0x00 });

    REQUIRE(onGetCommandScsi() == 1);
    CHECK(cmdLen == 6);
    CHECK(std::vector<uint8_t>(cmd, cmd + 6) == std::vector<uint8_t>{ 0x48, 0x00, 0x00, 0x10, 0x02, 0x00 });
    CHECK(fakeBridge.modes.back() == MODE_CMD);
}

TEST_CASE_FIXTURE(ScsiFixture, "10 byte command is converted to ICD form (0x1f prefix), 11 bytes") {
    std::vector<uint8_t> read10 = { 0x28, 0x00, 0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x08, 0x00 };
    fakeBridge.selectedId = 3;
    fromAtari(read10);

    REQUIRE(onGetCommandScsi() == 1);
    CHECK(cmdLen == 11);
    CHECK(cmd[0] == (0x1f | (3 << 5)));
    CHECK(std::vector<uint8_t>(cmd + 1, cmd + 11) == read10);
}

TEST_CASE_FIXTURE(ScsiFixture, "12 byte command is converted to ICD form, 13 bytes") {
    std::vector<uint8_t> read12 = { 0xa8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    fakeBridge.selectedId = 0;
    fromAtari(read12);

    REQUIRE(onGetCommandScsi() == 1);
    CHECK(cmdLen == 13);
    CHECK(cmd[0] == 0x1f);
    CHECK(std::vector<uint8_t>(cmd + 1, cmd + 13) == read12);
}

TEST_CASE_FIXTURE(ScsiFixture, "16 byte command is read whole, but forwarded truncated to the 14 B the host accepts") {
    std::vector<uint8_t> read16 = { 0x88, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    fakeBridge.selectedId = 1;
    fromAtari(read16);
    fromAtari({ 0x99 });                            // must not be read

    REQUIRE(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(fakeBridge.fromAtari.size() == 1);        // all 16 CDB bytes taken from the bus
    CHECK(cmdLen == ATN_SENDACSICOMMAND_LEN_TX - TX_HEADER_SIZE);

    // the whole CDB in ICD form is kept in cmd[]
    CHECK(cmd[0] == (0x1f | (1 << 5)));
    CHECK(std::vector<uint8_t>(cmd + 1, cmd + 17) == read16);

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);
    REQUIRE(msgs[0].length == 14);
    CHECK(msgs[0].data[0] == (0x1f | (1 << 5)));
    CHECK(std::vector<uint8_t>(msgs[0].data.begin() + 1, msgs[0].data.begin() + 14) == std::vector<uint8_t>(read16.begin(), read16.begin() + 13));
}

TEST_CASE_FIXTURE(ScsiFixture, "MESSAGE OUT byte while ATN is asserted is consumed before the command") {
    fakeBridge.selectedId  = 0;
    fakeBridge.currentMode = MODE_SCSI_SELECTION;
    fakeBridge.atn         = { true, false };       // ATN before the command, dropped after one message byte
    fromAtari({ 0x80 });                            // IDENTIFY message
    fromAtari({ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });

    REQUIRE(onGetCommandScsi() == 1);
    CHECK(std::vector<uint8_t>(cmd, cmd + 6) == std::vector<uint8_t>(6, 0x00));
    CHECK(fakeBridge.fromAtari.empty());

    REQUIRE(fakeBridge.modes.size() >= 3);
    CHECK(fakeBridge.modes[0] == MODE_MSG_OUT);
    CHECK(fakeBridge.modes[1] == MODE_SCSI_SELECTION);   // previous mode restored
    CHECK(fakeBridge.modes[2] == MODE_CMD);
}

TEST_CASE_FIXTURE(ScsiFixture, "MESSAGE OUT after the command is consumed too") {
    fakeBridge.selectedId = 0;
    fakeBridge.atn        = { false, true, false };    // none before, one after the command
    fromAtari({ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
    fromAtari({ 0x06 });                                // ABORT message

    REQUIRE(onGetCommandScsi() == 1);
    CHECK(fakeBridge.fromAtari.empty());
    CHECK(fakeBridge.modes.back() == MODE_CMD);         // mode restored after the message
}

TEST_CASE_FIXTURE(ScsiFixture, "timeout in the middle of the command fails and resets the bridge") {
    fakeBridge.selectedId = 0;
    fromAtari({ 0x28, 0x00, 0x00 });

    CHECK(onGetCommandScsi() == 0);
    CHECK(fakeBridge.resetCount == 1);
}

TEST_CASE_FIXTURE(ScsiFixture, "onGetCommand passes the command to core 0 and starts the timeout") {
    fakeBridge.selectedId = 4;
    fromAtari({ 0x12, 0x00, 0x00, 0x00, 0x24, 0x00 });     // INQUIRY

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(fakeActiveAlarms == 1);

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_GET_COMMAND);
    CHECK(msgs[0].length == 6);
    CHECK(msgs[0].data[0] == (0x12 | (4 << 5)));
}

TEST_CASE_FIXTURE(ScsiFixture, "onGetCommand failure stays in STATE_GET_COMMAND, nothing sent") {
    fakeBridge.selectedId = 0xff;

    timeoutStart(1000);
    CHECK(onGetCommand() == STATE_GET_COMMAND);
    CHECK(fakeActiveAlarms == 0);
    CHECK(drainIpc(0).empty());
}

TEST_CASE_FIXTURE(ScsiFixture, "onDataRead sends all bytes with DMA") {
    std::vector<uint8_t> data = { 9, 8, 7 };

    CHECK(onDataRead(data.size(), data.data()));
    CHECK(fakeBridge.dmaToAtari == data);
    CHECK(fakeBridge.modes.back() == MODE_DMA_READ);
}

TEST_CASE_FIXTURE(ScsiFixture, "onDataRead fails on DMA timeout") {
    std::vector<uint8_t> data(10, 0x11);
    fakeBridge.dmaReadFailAfter = 2;
    CHECK_FALSE(onDataRead(data.size(), data.data()));
}

TEST_CASE_FIXTURE(ScsiFixture, "onDataWrite announces the count, then forwards the data in 512 B chunks") {
    std::vector<uint8_t> data(1024 + 3);
    for (size_t i = 0; i < data.size(); i++) data[i] = i * 3;
    fromAtari(data);

    CHECK(onDataWrite(data.size()));
    CHECK(fakeBridge.dmaWriteCount == data.size());

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].command == STATE_SEND_WRITE_MORE_DATA);
    CHECK(msgs[0].length == data.size());

    std::vector<uint8_t> received;
    for (int i = 1; i < 4; i++) {
        CHECK(msgs[i].command == STATE_DATA_WRITE);
        received.insert(received.end(), msgs[i].data.begin(), msgs[i].data.end());
    }
    CHECK(msgs[3].length == 3);
    CHECK(received == data);
}

TEST_CASE_FIXTURE(ScsiFixture, "onDataWrite of 0 bytes sends just the header, no DMA") {
    fakeBridge.dmaWriteCount = 12345;

    CHECK(onDataWrite(0));

    CHECK(fakeBridge.dmaWriteCount == 12345);       // DMA_write_startWithCount() not called
    CHECK(fakeBridge.modes.empty());                // bus mode not touched

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);                      // host waits for ATN_WRITE_MORE_DATA even for 0 bytes
    CHECK(msgs[0].command == STATE_SEND_WRITE_MORE_DATA);
    CHECK(msgs[0].length == 0);
}

TEST_CASE_FIXTURE(ScsiFixture, "onDataWrite timeout does not lose an IPC buffer") {
    fromAtari(std::vector<uint8_t>(100, 0x22));
    REQUIRE_FALSE(onDataWrite(512));
    drainIpc(0);

    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {
        CAPTURE(i);
        CHECK(buffersForCore0[i].free);
    }
}

TEST_CASE_FIXTURE(ScsiFixture, "onReadStatus sends status (and message) phase") {
    onReadStatus(0x02);
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ 0x02 });
}

}
