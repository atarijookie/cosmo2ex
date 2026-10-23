// ce_pico_hdd/command_handling.cpp - the merged firmware takes the ACSI or the SCSI path by the board variant
// (the ACSI and SCSI behaviour itself is covered by tests/acsi and tests/scsi, which run also against ce_pico_hdd)

#include "test_helpers.h"
#include <bridge_fake.h>
#include "bridge.h"

namespace {

struct VariantFixture : FirmwareFixture {
    VariantFixture() { fakeBridge = FakeBridge(); }
};

}

TEST_SUITE("hdd variant") {

TEST_CASE_FIXTURE(VariantFixture, "ACSI variant reads the ID from the 1st command byte, ignores SCSI selection") {
    hwVariant = VARIANT_ACSI;
    settings.enabledIDs = 0x02;
    fakeBridge.selectedId = 0xff;                                       // no SCSI selection
    fakeBridge.fromAtari = { 0x28, 0x00, 0x00, 0x10, 0x01, 0x00 };      // ID 1, READ(6)

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(cmdLen == 6);
    CHECK(cmd[0] == 0x28);
    CHECK(fakeBridge.modes.empty());                                    // no SCSI phases

    auto msgs = drainIpc(0);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_GET_COMMAND);
}

TEST_CASE_FIXTURE(VariantFixture, "SCSI variant takes the ID from selection, ACSI style bytes alone are no command") {
    hwVariant = VARIANT_SCSI;
    settings.enabledIDs = 0x02;
    fakeBridge.selectedId = 0xff;                                       // not selected
    fakeBridge.fromAtari = { 0x28, 0x00, 0x00, 0x10, 0x01, 0x00 };

    CHECK(onGetCommand() == STATE_GET_COMMAND);
    CHECK(fakeBridge.fromAtari.size() == 6);                            // nothing read from the bus

    fakeBridge.selectedId = 1;                                          // now selected as ID 1
    fakeBridge.fromAtari = { 0x08, 0x00, 0x00, 0x10, 0x01, 0x00 };      // READ(6)

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(cmd[0] == (0x08 | (1 << 5)));                                 // ID added on top, like ACSI
    CHECK(fakeBridge.modes.back() == MODE_CMD);
}

TEST_CASE_FIXTURE(VariantFixture, "status goes out through statusAndMsgRead on both variants") {
    hwVariant = VARIANT_ACSI;
    onReadStatus(0x00);
    hwVariant = VARIANT_SCSI;
    onReadStatus(0x02);

    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ 0x00, 0x02 });
}

}
