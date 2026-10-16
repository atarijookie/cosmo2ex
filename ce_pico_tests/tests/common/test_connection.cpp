// connection.cpp - CE host discovery, connecting, parsing the commands coming from host, sending data to host

#include <set>
#include <string>
#include "test_helpers.h"

namespace {

void setConnected(void)
{
    connected = true;
    clientHdd.isConnected = true;
}

void setKnownHost(void)
{
    hostIp[0] = 10; hostIp[1] = 0; hostIp[2] = 0; hostIp[3] = 5;
    hostIpString = "10.0.0.5";
    hostPortHdd  = 1234;
    hostPortFdd  = 1235;
    hostPortIkbd = 1236;
}

std::vector<uint8_t> concat(std::vector<uint8_t> a, const std::vector<uint8_t> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}

TEST_SUITE("connection - header from host") {

TEST_CASE_FIXTURE(FirmwareFixture, "no header when not connected") {
    clientHdd.pushRx(hostHeader(CMD_SEND_STATUS, 1));

    THeader h = {};
    CHECK_FALSE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(clientHdd.rx.size() == 10);       // nothing consumed
}

TEST_CASE_FIXTURE(FirmwareFixture, "complete header is parsed") {
    setConnected();
    clientHdd.pushRx(hostHeader(0x1234, 0xa1b2c3d4));

    THeader h = {};
    REQUIRE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(h.syncTag == SYNC_TAG_HDD);
    CHECK(h.cmdCode == 0x1234);
    CHECK(h.len == 0xa1b2c3d4);
    CHECK(clientHdd.rx.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "incomplete header is not consumed until complete") {
    setConnected();
    auto hdr = hostHeader(CMD_SEND_STATUS, 7);
    clientHdd.pushRx(std::vector<uint8_t>(hdr.begin(), hdr.begin() + 9));

    THeader h = {};
    CHECK_FALSE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(clientHdd.rx.size() == 9);

    clientHdd.pushRx({ hdr[9] });
    REQUIRE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(h.cmdCode == CMD_SEND_STATUS);
    CHECK(h.len == 7);
}

TEST_CASE_FIXTURE(FirmwareFixture, "garbage before the sync tag is skipped, including a false start") {
    setConnected();
    clientHdd.pushRx(concat({ 0x01, 0xc0, 0x50, 0x02, 0xc0, 0xc0, 0x50, 0xd1, 0x00 }, hostHeader(CMD_DATA_WRITE, 4)));

    THeader h = {};
    REQUIRE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(h.cmdCode == CMD_DATA_WRITE);
    CHECK(h.len == 4);
    CHECK(clientHdd.rx.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "resync works when the sync tag is split between two calls") {
    setConnected();
    auto hdr = hostHeader(CMD_DATA_MARKER, 0x200);

    // 9 bytes of garbage + first 3 bytes of the tag: the garbage gets consumed, the partial tag stays in header->syncTag
    clientHdd.pushRx(concat(std::vector<uint8_t>(9, 0x11), std::vector<uint8_t>(hdr.begin(), hdr.begin() + 3)));

    THeader h = {};
    CHECK_FALSE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));

    clientHdd.pushRx(std::vector<uint8_t>(hdr.begin() + 3, hdr.end()));
    REQUIRE(getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &h));
    CHECK(h.cmdCode == CMD_DATA_MARKER);
    CHECK(h.len == 0x200);
    CHECK(clientHdd.rx.empty());
}

}

TEST_SUITE("connection - commands from host") {

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG stores new enabled IDs") {
    setConnected();
    settings.enabledIDs = 0x01;
    clientHdd.pushRx(concat(hostHeader(CMD_ACSI_CONFIG, 2), { CMD_ACSI_CONFIG, 0x05 }));

    handleIncommingData();

    CHECK(settings.enabledIDs == 0x05);
    CHECK(EEPROM.commitCount == 1);
    CHECK(clientHdd.rx.empty());
    CHECK(hddHeader.syncTag == 0);      // cleared for the next header
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG with unchanged IDs does not write flash") {
    setConnected();
    settings.enabledIDs = 0x05;
    clientHdd.pushRx(concat(hostHeader(CMD_ACSI_CONFIG, 2), { CMD_ACSI_CONFIG, 0x05 }));

    handleIncommingData();

    CHECK(settings.enabledIDs == 0x05);
    CHECK(EEPROM.commitCount == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG skips other bytes in the payload") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_ACSI_CONFIG, 6), { 0x00, 0x99, CMD_ACSI_CONFIG, 0x82, 0x00, 0x00 }));

    handleIncommingData();

    CHECK(settings.enabledIDs == 0x82);
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG longer than the 32 B buffer - extra bytes are dropped") {
    setConnected();
    std::vector<uint8_t> payload(48, 0x00);
    payload[0] = CMD_ACSI_CONFIG;
    payload[1] = 0x03;
    clientHdd.pushRx(concat(hostHeader(CMD_ACSI_CONFIG, payload.size()), payload));

    handleIncommingData();

    CHECK(settings.enabledIDs == 0x03);
    CHECK(clientHdd.rx.empty());        // the rest of the payload must not be left in the stream either
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG longer than the 32 B buffer - next header is still found") {
    setConnected();
    std::vector<uint8_t> payload(40, 0x00);
    payload[0] = CMD_ACSI_CONFIG;
    payload[1] = 0x03;
    payload[35] = CMD_ACSI_CONFIG;      // beyond the buffer - ignored
    payload[36] = 0x7f;
    clientHdd.pushRx(concat(hostHeader(CMD_ACSI_CONFIG, payload.size()), payload));
    clientHdd.pushRx(concat(hostHeader(CMD_SEND_STATUS, 1), { 0x02 }));

    handleIncommingData();
    handleIncommingData();

    CHECK(settings.enabledIDs == 0x03);
    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_READ_STATUS);
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_ACSI_CONFIG marker as the last byte, without value, is ignored") {
    setConnected();
    std::vector<uint8_t> payload(32, 0x00);
    payload[31] = CMD_ACSI_CONFIG;      // no value byte follows
    clientHdd.pushRx(payload);          // header already taken by handleIncommingData()
    clientHdd.pushRx({ 0x7f });         // whatever comes next in the stream must not be taken as the value

    handleAcsiConfig(32);

    CHECK(settings.enabledIDs == 0x01);
    CHECK(clientHdd.rx.size() == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_SEND_STATUS passes the status byte to core 1") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_SEND_STATUS, 1), { 0x02 }));

    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_READ_STATUS);
    CHECK(msgs[0].length == 1);
    CHECK(msgs[0].data[0] == 0x02);
    CHECK(drainIpc(0).empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "CMD_DATA_WRITE passes the 24 bit byte count to core 1") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_WRITE, 4), { 0x01, 0x02, 0x00, 0x00 }));

    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_DATA_WRITE);
    CHECK(msgs[0].length == 0x010200);
}

TEST_CASE_FIXTURE(FirmwareFixture, "read with status - data split into 512 B buffers, then the status") {
    setConnected();

    std::vector<uint8_t> data(1100);
    for (size_t i = 0; i < data.size(); i++) data[i] = i * 7;

    clientHdd.pushRx(concat(hostHeader(CMD_DATA_READ_WITH_STATUS, 4), { 0x00, 0x04, 0x4c, 0x00 }));    // status byte 0x00 (OK)
    clientHdd.pushRx(hostHeader(CMD_DATA_MARKER, data.size()));
    clientHdd.pushRx(data);

    handleIncommingData();              // read start
    CHECK(drainIpc(1).empty());         // nothing for core 1 yet

    handleIncommingData();              // data
    auto msgs = drainIpc(1);

    REQUIRE(msgs.size() == 4);
    std::vector<uint8_t> received;
    uint32_t sizes[3] = { 512, 512, 76 };
    for (int i = 0; i < 3; i++) {
        CAPTURE(i);
        CHECK(msgs[i].command == STATE_DATA_READ);
        CHECK(msgs[i].length == sizes[i]);
        received.insert(received.end(), msgs[i].data.begin(), msgs[i].data.end());
    }
    CHECK(received == data);

    CHECK(msgs[3].command == STATE_READ_STATUS);
    CHECK(msgs[3].data[0] == 0x00);
    CHECK(clientHdd.rx.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "read without status - no status buffer after the data") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_READ_WITHOUT_STATUS, 4), { 0x00, 0x00, 0x10, 0x02 }));
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_MARKER, 16), std::vector<uint8_t>(16, 0xee)));

    handleIncommingData();
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_DATA_READ);
    CHECK(msgs[0].length == 16);
}

TEST_CASE_FIXTURE(FirmwareFixture, "read status byte comes from the read start command") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_READ_WITH_STATUS, 4), { 0x00, 0x00, 0x01, 0x02 }));
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_MARKER, 1), { 0x5a }));

    handleIncommingData();
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[1].command == STATE_READ_STATUS);
    CHECK(msgs[1].data[0] == 0x02);
}

TEST_CASE_FIXTURE(FirmwareFixture, "read data that stops arriving times out without bogus buffers") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_READ_WITH_STATUS, 4), { 0, 0, 0, 0 }));
    clientHdd.pushRx(hostHeader(CMD_DATA_MARKER, 600));
    clientHdd.pushRx(std::vector<uint8_t>(100, 0x44));     // only 100 of 600 bytes ever come

    handleIncommingData();

    fakeMillisStep = 1;                 // let the 5 s timeout expire
    uint32_t start = fakeNow;
    handleIncommingData();

    CHECK(fakeNow - start >= 5000);

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 2);          // the data that came + the status
    CHECK(msgs[0].command == STATE_DATA_READ);
    CHECK(msgs[0].length == 100);
    CHECK(msgs[1].command == STATE_READ_STATUS);

    for (int i = 0; i < IPC_BUFFER_COUNT; i++) {    // no buffer lost on the timeout
        CAPTURE(i);
        CHECK(buffersForCore1[i].free);
    }
}

TEST_CASE_FIXTURE(FirmwareFixture, "command is handled only when its payload arrived") {
    setConnected();
    clientHdd.pushRx(hostHeader(CMD_DATA_WRITE, 4));
    clientHdd.pushRx({ 0x00, 0x02 });               // only half of the payload so far

    handleIncommingData();
    CHECK(drainIpc(1).empty());
    CHECK(clientHdd.rx.size() == 2);                // payload not consumed

    fakeAdvanceMillis(500);
    clientHdd.pushRx({ 0x00, 0xff });
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_DATA_WRITE);
    CHECK(msgs[0].length == 0x000200);
    CHECK(clientHdd.rx.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "status byte arriving separately from its header") {
    setConnected();
    clientHdd.pushRx(hostHeader(CMD_SEND_STATUS, 1));

    handleIncommingData();
    CHECK(drainIpc(1).empty());

    clientHdd.pushRx({ 0x02 });
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].data[0] == 0x02);                 // not 0xff from reading an empty socket
}

TEST_CASE_FIXTURE(FirmwareFixture, "header whose payload never comes is dropped, the next command still works") {
    setConnected();
    clientHdd.pushRx(hostHeader(CMD_DATA_READ_WITH_STATUS, 4));
    clientHdd.pushRx({ 0x00 });                     // 3 bytes missing

    handleIncommingData();
    fakeAdvanceMillis(1000);
    handleIncommingData();
    CHECK(hddHeaderPending);                        // still waiting

    fakeAdvanceMillis(1);
    handleIncommingData();
    CHECK_FALSE(hddHeaderPending);                  // gave up
#ifdef LOG_LED
    CHECK(fakeDebugOut.find("didn't arrive") != std::string::npos);
#endif

    clientHdd.pushRx(concat(hostHeader(CMD_SEND_STATUS, 1), { 0x00 }));
    handleIncommingData();                          // the left over byte is skipped while looking for sync tag

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].command == STATE_READ_STATUS);
}

TEST_CASE_FIXTURE(FirmwareFixture, "data marker is handled without waiting for all the data") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_DATA_READ_WITHOUT_STATUS, 4), { 0, 0, 0x04, 0 }));
    handleIncommingData();

    clientHdd.pushRx(hostHeader(CMD_DATA_MARKER, 1024));
    clientHdd.pushRx(std::vector<uint8_t>(1024, 0x77));    // all there, but more than PAYLOAD_MAX_WAIT_SIZE
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].length + msgs[1].length == 1024);
}

TEST_CASE_FIXTURE(FirmwareFixture, "big ACSI config payload waits only for what fits the buffer") {
    setConnected();
    std::vector<uint8_t> payload(200, 0x00);
    payload[0] = CMD_ACSI_CONFIG;
    payload[1] = 0x06;
    clientHdd.pushRx(hostHeader(CMD_ACSI_CONFIG, payload.size()));
    clientHdd.pushRx(std::vector<uint8_t>(payload.begin(), payload.begin() + 64));

    handleIncommingData();
    CHECK(settings.enabledIDs == 0x06);
}

TEST_CASE_FIXTURE(FirmwareFixture, "two headers in a row are both handled") {
    setConnected();
    clientHdd.pushRx(concat(hostHeader(CMD_SEND_STATUS, 1), { 0x00 }));
    clientHdd.pushRx(concat(hostHeader(CMD_SEND_STATUS, 1), { 0x02 }));

    handleIncommingData();
    handleIncommingData();

    auto msgs = drainIpc(1);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].data[0] == 0x00);
    CHECK(msgs[1].data[0] == 0x02);
}

TEST_CASE_FIXTURE(FirmwareFixture, "unknown command is reported and ignored") {
    setConnected();
    clientHdd.pushRx(hostHeader(0x99, 0));

    handleIncommingData();

    CHECK(drainIpc(1).empty());
#ifdef LOG_LED
    CHECK(fakeDebugOut.find("unknown cmdCode 153") != std::string::npos);
#endif
}

}

TEST_SUITE("connection - DHCP") {

TEST_CASE_FIXTURE(FirmwareFixture, "DHCP lease is maintained once per second") {
    dhcpMaintain();
    CHECK(Ethernet.maintainCount == 1);

    dhcpMaintain();
    fakeAdvanceMillis(999);
    dhcpMaintain();
    CHECK(Ethernet.maintainCount == 1);

    fakeAdvanceMillis(1);
    dhcpMaintain();
    CHECK(Ethernet.maintainCount == 2);
}

TEST_CASE_FIXTURE(FirmwareFixture, "connectToHost maintains the DHCP lease") {
    setKnownHost();
    setConnected();

    connectToHost();
    CHECK(Ethernet.maintainCount == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "renewal with the same IP keeps the connection") {
    setConnected();
    Ethernet.maintainResult = DHCP_CHECK_RENEW_OK;

    dhcpMaintain();

    CHECK(connected);
    CHECK(clientHdd.stopCount == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "rebind with a new IP closes the connection, so it reconnects") {
    setKnownHost();
    setConnected();
    Ethernet.maintainResult = DHCP_CHECK_REBIND_OK;
    Ethernet.maintainNewIp  = IPAddress(192, 168, 1, 77);

    dhcpMaintain();

    CHECK_FALSE(connected);
    CHECK(clientHdd.stopCount == 1);
#ifdef LOG_LED
    CHECK(fakeDebugOut.find("192.168.1.50 to 192.168.1.77") != std::string::npos);
#endif

    connectToHost();                    // reconnects to the same host
    CHECK(clientHdd.connectCount == 1);
    CHECK(connected);
}

TEST_CASE_FIXTURE(FirmwareFixture, "failed renewal is reported, connection kept") {
    setConnected();
    Ethernet.maintainResult = DHCP_CHECK_RENEW_FAIL;

    dhcpMaintain();

    CHECK(connected);
#ifdef LOG_LED
    CHECK(fakeDebugOut.find("renewal failed") != std::string::npos);
#endif
}

}

TEST_SUITE("connection - sending to host") {

TEST_CASE_FIXTURE(FirmwareFixture, "sendHeaderAndDataToHost stores the length and sends header + data") {
    setConnected();
    uint8_t bfr[TX_HEADER_SIZE + 4];
    storeHeader(bfr, ATN_ACSI_COMMAND, 0);
    bfr[10] = 1; bfr[11] = 2; bfr[12] = 3; bfr[13] = 4;

    CHECK(sendHeaderAndDataToHost(SOCK_HDD, bfr, 4));

    REQUIRE(clientHdd.tx.size() == TX_HEADER_SIZE + 4);
    CHECK(getDword(clientHdd.tx.data()) == SYNC_TAG_HDD);
    CHECK(getWord(clientHdd.tx.data() + 4) == ATN_ACSI_COMMAND);
    CHECK(getDword(clientHdd.tx.data() + 6) == 4);
    CHECK(clientHdd.tx[13] == 4);
}

TEST_CASE_FIXTURE(FirmwareFixture, "sendDataToHost selects the socket") {
    connected = true;
    clientHdd.isConnected = clientFdd.isConnected = clientIkbd.isConnected = true;
    uint8_t b = 0x42;

    CHECK(sendDataToHost(SOCK_FDD, &b, 1));
    CHECK(sendDataToHost(SOCK_IKBD, &b, 1));
    CHECK(clientHdd.tx.empty());
    CHECK(clientFdd.tx.size() == 1);
    CHECK(clientIkbd.tx.size() == 1);

    CHECK_FALSE(sendDataToHost(7, &b, 1));
}

TEST_CASE_FIXTURE(FirmwareFixture, "sendDataToHost fails when not connected or write is short") {
    uint8_t b[4] = {};

    clientHdd.isConnected = true;
    connected = false;
    CHECK_FALSE(sendDataToHost(SOCK_HDD, b, 4));
    CHECK(clientHdd.tx.empty());

    connected = true;
    clientHdd.isConnected = false;      // socket dropped, write returns 0
    CHECK_FALSE(sendDataToHost(SOCK_HDD, b, 4));
}

}

TEST_SUITE("connection - discovery") {

TEST_CASE_FIXTURE(FirmwareFixture, "discovery broadcasts CELC + mac, alternating subnet and global broadcast") {
    uint8_t mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    memcpy(settings.mac, mac, 6);

    ceDiscoverySend();
    ceDiscoverySend();                  // within 1 s - throttled
    REQUIRE(udp.sent.size() == 1);

    fakeAdvanceMillis(1000);
    ceDiscoverySend();
    REQUIRE(udp.sent.size() == 2);

    std::set<std::string> hosts;
    for (auto &p : udp.sent) {
        hosts.insert(p.host);
        CHECK(p.port == 7200);
        REQUIRE(p.data.size() == 10);
        CHECK(memcmp(p.data.data(), "CELC", 4) == 0);
        CHECK(memcmp(p.data.data() + 4, mac, 6) == 0);
    }

    CHECK(hosts == std::set<std::string>{ "192.168.1.255", "255.255.255.255" });
}

TEST_CASE_FIXTURE(FirmwareFixture, "discovery subnet broadcast follows the netmask") {
    Ethernet.ip   = IPAddress(10, 1, 2, 3);
    Ethernet.mask = IPAddress(255, 255, 240, 0);

    ceDiscoverySend();
    fakeAdvanceMillis(1000);
    ceDiscoverySend();

    std::set<std::string> hosts;
    for (auto &p : udp.sent) hosts.insert(p.host);
    CHECK(hosts.count("10.1.15.255") == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "no discovery broadcast when the host is already known") {
    setKnownHost();
    ceDiscoverySend();
    CHECK(udp.sent.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "CELR response sets host ip and ports, other packets are ignored") {
    udp.incoming.push_back({ "10.9.9.9", 7201, { 'J', 'U', 'N', 'K', 0, 1, 0, 2, 0, 3 } });
    udp.incoming.push_back({ "10.0.0.7", 7201, { 'C', 'E', 'L', 'R', 0x1f, 0x90, 0x1f, 0x91, 0x1f, 0x92 } });

    ceDiscoveryReceive();

    CHECK(hostIp[0] == 10); CHECK(hostIp[1] == 0); CHECK(hostIp[2] == 0); CHECK(hostIp[3] == 7);
    CHECK(hostIpString == "10.0.0.7");
    CHECK(hostPortHdd == 0x1f90);
    CHECK(hostPortFdd == 0x1f91);
    CHECK(hostPortIkbd == 0x1f92);
    CHECK(udp.incoming.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "only junk received - host stays unknown") {
    udp.incoming.push_back({ "10.9.9.9", 7201, { 'C', 'E', 'L', 'C', 0, 1, 0, 2, 0, 3 } });

    ceDiscoveryReceive();

    CHECK(hostIp[0] == 0);
    CHECK(hostPortHdd == 0);
    CHECK(hostIpString.length() == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "connectToHost without a host opens udp and does discovery") {
    connectToHost();

    CHECK(udp.begun);
    CHECK(udp.localPort == 7201);
    CHECK(udp.sent.size() == 1);
    CHECK(clientHdd.connectCount == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "connectToHost with a known host closes udp and connects, retrying every 3 s") {
    setKnownHost();
    udpInitialized = true;
    udp.begun = true;
    clientHdd.connectResult = false;

    connectToHost();
    CHECK_FALSE(udp.begun);
    CHECK(clientHdd.connectCount == 1);
    CHECK(clientHdd.connectHost == "10.0.0.5");
    CHECK(clientHdd.connectPort == 1234);
    CHECK_FALSE(connected);

    fakeAdvanceMillis(2999);
    connectToHost();
    CHECK(clientHdd.connectCount == 1);

    clientHdd.connectResult = true;
    fakeAdvanceMillis(1);
    connectToHost();
    CHECK(clientHdd.connectCount == 2);
    CHECK(connected);

    fakeAdvanceMillis(5000);
    connectToHost();                    // stays connected, no reconnect
    CHECK(clientHdd.connectCount == 2);
}

TEST_CASE_FIXTURE(FirmwareFixture, "host is forgotten after 3 failed connects, so it gets discovered again") {
    setKnownHost();
    clientHdd.connectResult = false;

    connectToHost();
    fakeAdvanceMillis(3000);
    connectToHost();
    CHECK(clientHdd.connectCount == 2);
    CHECK(hostPortHdd == 1234);                     // 2 failures - still the same host

    fakeAdvanceMillis(3000);
    connectToHost();
    CHECK(clientHdd.connectCount == 3);
    CHECK(hostIp[0] == 0);                          // 3 failures - forgotten
    CHECK(hostIpString.length() == 0);
    CHECK(hostPortHdd == 0);
    CHECK(hostPortFdd == 0);
    CHECK(hostPortIkbd == 0);

    // the next round discovers the host on its new address
    udp.incoming.push_back({ "10.0.0.9", 7201, { 'C', 'E', 'L', 'R', 0x04, 0xd3, 0, 0, 0, 0 } });
    clientHdd.connectResult = true;
    fakeAdvanceMillis(3000);
    connectToHost();                                // discovery receive (+ broadcast)
    connectToHost();                                // connect

    CHECK(udp.begun == false);
    CHECK(clientHdd.connectHost == "10.0.0.9");
    CHECK(clientHdd.connectPort == 1235);
    CHECK(connected);
}

TEST_CASE_FIXTURE(FirmwareFixture, "successful connect resets the failure count") {
    setKnownHost();

    clientHdd.connectResult = false;
    connectToCEhost();
    fakeAdvanceMillis(3000);
    connectToCEhost();
    CHECK(connectFailCount == 2);

    clientHdd.connectResult = true;
    fakeAdvanceMillis(3000);
    connectToCEhost();
    CHECK(connectFailCount == 0);
    CHECK(hostPortHdd == 1234);
}

TEST_CASE_FIXTURE(FirmwareFixture, "new connection starts with clean stream state") {
    setKnownHost();
    hddHeader.syncTag = 0x00c050d1;                 // partial sync tag from the previous connection
    hddHeaderPending  = true;

    connectToCEhost();

    CHECK(connected);
    CHECK(hddHeader.syncTag == 0);
    CHECK_FALSE(hddHeaderPending);
}

TEST_CASE_FIXTURE(FirmwareFixture, "discovery response leads to connection") {
    udp.incoming.push_back({ "10.0.0.8", 7201, { 'C', 'E', 'L', 'R', 0x04, 0xd2, 0, 0, 0, 0 } });
    udpInitialized = true;
    hostIp[0] = 0;

    ceDiscoveryReceive();
    connectToCEhost();

    CHECK(clientHdd.connectHost == "10.0.0.8");
    CHECK(clientHdd.connectPort == 1234);
}

}
