// ce_pico_hdd/boot_status.cpp - while not connected to the host, core 1 answers the commands itself: boot status root sector
// on sector 0 of the lowest enabled ID, the status on sector 1, NOT READY for the rest

#include <string>

#include "test_helpers.h"
#include <bridge_fake.h>
#include "bridge.h"
#include "scsi.h"
#include "boot_status.h"
#include "boot_status_data.h"

namespace {

struct BootStatusFixture : FirmwareFixture {
    BootStatusFixture() {
        fakeBridge = FakeBridge();
        hwVariant = VARIANT_ACSI;
        connected = false;
        settings.enabledIDs = 0x0c;         // IDs 2, 3 - boot ID is 2
    }
};

uint16_t checksum(const uint8_t* s)
{
    uint16_t sum = 0;
    for(int i = 0; i < 512; i += 2) {
        sum += (s[i] << 8) | s[i + 1];
    }
    return sum;
}

std::string statusText(const std::vector<uint8_t>& sector)
{
    return std::string((const char*) sector.data() + 16);
}

// ACSI READ(6) of ID, sector, count
void acsiRead6(uint8_t id, uint8_t sector, uint8_t count)
{
    fakeBridge.fromAtari = { (uint8_t) ((id << 5) | 0x08), 0x00, 0x00, sector, count, 0x00 };
}

}

TEST_SUITE("boot status") {

TEST_CASE_FIXTURE(BootStatusFixture, "sector 0 of the lowest enabled ID is the executable boot status sector with its devno") {
    acsiRead6(2, 0, 1);

    CHECK(onGetCommand() == STATE_GET_COMMAND);         // answered here
    CHECK(drainIpc(0).empty());                         // nothing to host
    REQUIRE(fakeBridge.dmaToAtari.size() == 512);
    CHECK(checksum(fakeBridge.dmaToAtari.data()) == 0x1234);
    CHECK(fakeBridge.dmaToAtari[BOOT_STATUS_DEVNO_OFFSET] == 2);
    CHECK(fakeBridge.dmaToAtari[0] == bootStatusSector[0]);
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ SCSI_ST_OK });
}

TEST_CASE_FIXTURE(BootStatusFixture, "SCSI: devno is 8 + ID, READ(10) works too") {
    hwVariant = VARIANT_SCSI;
    fakeBridge.selectedId = 2;
    fakeBridge.fromAtari = { 0x28, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x02, 0x00 };     // READ(10) sector 0, 2 sectors

    CHECK(onGetCommand() == STATE_GET_COMMAND);
    REQUIRE(fakeBridge.dmaToAtari.size() == 1024);
    CHECK(fakeBridge.dmaToAtari[BOOT_STATUS_DEVNO_OFFSET] == 8 + 2);
    CHECK(checksum(fakeBridge.dmaToAtari.data()) == 0x1234);

    std::vector<uint8_t> status(fakeBridge.dmaToAtari.begin() + 512, fakeBridge.dmaToAtari.end());
    CHECK(std::string((const char*) status.data(), 8) == "CE2XSTAT");
    CHECK(statusText(status).find("SCSI ID 2") != std::string::npos);
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ SCSI_ST_OK });
}

TEST_CASE_FIXTURE(BootStatusFixture, "sector 1 is the status - code, counter and the text change with the state") {
    uint8_t ip[4] = { 192, 168, 1, 50 };
    uint8_t host[4] = { 192, 168, 1, 2 };
    bootStatusSetOwnIp(ip);
    bootStatusSet(BOOTST_SEARCHING);

    acsiRead6(2, 1, 1);
    onGetCommand();
    REQUIRE(fakeBridge.dmaToAtari.size() == 512);
    std::vector<uint8_t> first = fakeBridge.dmaToAtari;
    CHECK(first[8] == BOOTST_SEARCHING);
    CHECK(statusText(first).find("192.168.1.50") != std::string::npos);
    CHECK(statusText(first).find("Looking for") != std::string::npos);
    CHECK(statusText(first).find("ACSI ID 2") != std::string::npos);
    CHECK(first[511] == 0);

    bootStatusSet(BOOTST_SEARCHING);                    // same state - same counter
    fakeBridge.dmaToAtari.clear();
    acsiRead6(2, 1, 1);
    onGetCommand();
    CHECK(fakeBridge.dmaToAtari[9] == first[9]);

    bootStatusSet(BOOTST_CONNECTING, host);             // changed - new counter, host IP in the text
    fakeBridge.dmaToAtari.clear();
    acsiRead6(2, 1, 1);
    onGetCommand();
    CHECK(fakeBridge.dmaToAtari[8] == BOOTST_CONNECTING);
    CHECK(fakeBridge.dmaToAtari[9] != first[9]);
    CHECK(statusText(fakeBridge.dmaToAtari).find("192.168.1.2\r") != std::string::npos);
}

TEST_CASE_FIXTURE(BootStatusFixture, "other sectors, other IDs and other commands get CHECK CONDITION, no data") {
    SUBCASE("sector 2")                 { acsiRead6(2, 2, 1); }
    SUBCASE("sectors 1..32 (CE_DD)")    { acsiRead6(2, 1, 32); }
    SUBCASE("sectors 0..2")             { acsiRead6(2, 0, 3); }
    SUBCASE("not the boot ID")          { acsiRead6(3, 0, 1); }
    SUBCASE("TEST UNIT READY")          { fakeBridge.fromAtari = { 0x40, 0, 0, 0, 0, 0 }; }
    SUBCASE("ICD READ(10) far away")    { fakeBridge.fromAtari = { 0x5f, 0x28, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 2, 0 }; }

    CHECK(onGetCommand() == STATE_GET_COMMAND);
    CHECK(drainIpc(0).empty());
    CHECK(fakeBridge.dmaToAtari.empty());
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ SCSI_ST_CHECK_CONDITION });
}

TEST_CASE_FIXTURE(BootStatusFixture, "REQUEST SENSE says NOT READY, becoming ready") {
    fakeBridge.fromAtari = { 0x43, 0, 0, 0, 18, 0 };    // ID 2

    onGetCommand();
    REQUIRE(fakeBridge.dmaToAtari.size() == 18);
    CHECK(fakeBridge.dmaToAtari[2] == SCSI_E_NotReady);
    CHECK(fakeBridge.dmaToAtari[12] == SCSI_ASC_LU_NOT_READY);
    CHECK(fakeBridge.dmaToAtari[13] == 0x01);
    CHECK(fakeBridge.statusToAtari == std::vector<uint8_t>{ SCSI_ST_OK });
}

TEST_CASE_FIXTURE(BootStatusFixture, "STinG / MiNTnet commands still go to core 0") {
    fakeBridge.fromAtari = { 0x40, 'C', 'E', 0x07, 0x00, 0x00 };   // ID 2, HOSTMOD_RAW_NET IDENTIFY

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(drainIpc(0).size() == 1);
    CHECK(fakeBridge.statusToAtari.empty());
}

TEST_CASE_FIXTURE(BootStatusFixture, "when connected, sector 0 goes to the host") {
    connected = true;
    acsiRead6(2, 0, 1);

    CHECK(onGetCommand() == STATE_WAIT_COMMAND_RESPONSE);
    CHECK(drainIpc(0).size() == 1);
    CHECK(fakeBridge.dmaToAtari.empty());
}

}
