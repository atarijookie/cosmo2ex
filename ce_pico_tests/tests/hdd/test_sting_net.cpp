// sting_net.cpp - the HOSTMOD_RAW_NET commands of CE_ETH.STX and the raw ethernet side, against the fake EthernetMacraw.

#include "test_helpers.h"
#include "sting_net.h"

static const uint8_t STING_IP[4]  = { 192, 168, 1, 77 };
static const uint8_t PEER_IP[4]   = { 192, 168, 1, 10 };
static const uint8_t PEER_MAC[6]  = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t BCAST_MAC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

struct Reply {
    std::vector<uint8_t> data;      // all STATE_DATA_READ data
    int                  status = -1;
    uint32_t             writeCount = 0;    // STATE_DATA_WRITE length, 0 if none
};

static Reply takeReply(void)
{
    Reply r;
    for (auto &m : drainIpc(1)) {
        if (m.command == STATE_DATA_READ)   r.data.insert(r.data.end(), m.data.begin(), m.data.end());
        if (m.command == STATE_READ_STATUS) r.status = m.data[0];
        if (m.command == STATE_DATA_WRITE)  r.writeCount = m.length;
    }
    return r;
}

static Reply shortCmd(uint8_t func)
{
    uint8_t c[6] = { 0x00, 'C', 'E', HOSTMOD_RAW_NET, func, 0 };
    REQUIRE(stingNetCommand(c, 6));
    return takeReply();
}

static Reply icdCmd(uint8_t func, uint8_t p0 = 0, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0, uint8_t p4 = 0)
{
    uint8_t c[11] = { 0x1f, 0x20, 'C', 'E', HOSTMOD_RAW_NET, func, p0, p1, p2, p3, p4 };
    REQUIRE(stingNetCommand(c, 11));
    return takeReply();
}

static Reply configEnable(uint8_t flags = 0x01)
{
    return icdCmd(RAWNET_CMD_CONFIG, flags, STING_IP[0], STING_IP[1], STING_IP[2], STING_IP[3]);
}

static std::vector<uint8_t> stingMac(void)
{
    Reply r = shortCmd(RAWNET_CMD_IDENTIFY);
    return std::vector<uint8_t>(r.data.begin() + 10, r.data.begin() + 16);
}

// network off + on, so each test starts with empty queues and ARP cache
static void netStart(uint8_t flags = 0x01)
{
    icdCmd(RAWNET_CMD_CONFIG, 0);
    EthernetMacraw::resetFake();
    REQUIRE(configEnable(flags).status == 0);
    EthernetMacraw::sent.clear();           // the gratuitous ARP
}

static std::vector<uint8_t> ipPacket(const uint8_t *src, const uint8_t *dst, uint16_t payloadLen)
{
    std::vector<uint8_t> p(20 + payloadLen, 0);
    p[0] = 0x45;
    p[2] = (20 + payloadLen) >> 8;
    p[3] = (20 + payloadLen) & 0xff;
    p[8] = 64;
    p[9] = 17;
    memcpy(&p[12], src, 4);
    memcpy(&p[16], dst, 4);
    for (int i = 0; i < payloadLen; i++) p[20 + i] = (uint8_t) i;
    return p;
}

static std::vector<uint8_t> ethFrame(const uint8_t *dst, const uint8_t *src, uint16_t type, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> f(dst, dst + 6);
    f.insert(f.end(), src, src + 6);
    f.push_back(type >> 8);
    f.push_back(type & 0xff);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

static std::vector<uint8_t> arpPayload(uint16_t op, const uint8_t *sha, const uint8_t *spa, const uint8_t *tha, const uint8_t *tpa)
{
    std::vector<uint8_t> a = { 0, 1, 0x08, 0x00, 6, 4, 0, (uint8_t) op };
    a.insert(a.end(), sha, sha + 6);
    a.insert(a.end(), spa, spa + 4);
    a.insert(a.end(), tha, tha + 6);
    a.insert(a.end(), tpa, tpa + 4);
    return a;
}

// SEND batch: [len16][next hop][packet], padded to even
static std::vector<uint8_t> sendEntry(const uint8_t *nextHop, const std::vector<uint8_t> &pkt)
{
    std::vector<uint8_t> e = { (uint8_t)(pkt.size() >> 8), (uint8_t) pkt.size() };
    e.insert(e.end(), nextHop, nextHop + 4);
    e.insert(e.end(), pkt.begin(), pkt.end());
    if (e.size() & 1) e.push_back(0);
    return e;
}

static Reply doSend(const std::vector<uint8_t> &batchData, uint8_t batchNo)
{
    uint32_t n = batchData.size();
    Reply r = icdCmd(RAWNET_CMD_SEND, n >> 16, n >> 8, n, batchNo, 1);
    REQUIRE(r.writeCount == n);
    REQUIRE(stingNetOwnsWrite());

    std::vector<uint8_t> copy = batchData;
    stingNetWriteData(copy.data(), copy.size());
    CHECK_FALSE(stingNetOwnsWrite());
    return takeReply();
}

//--------------------------------------------------------

TEST_CASE_FIXTURE(FirmwareFixture, "commands of other host modules are not taken") {
    uint8_t translated[6] = { 0x00, 'C', 'E', 3, 0, 0 };
    CHECK_FALSE(stingNetCommand(translated, 6));

    uint8_t read6[6] = { 0x08, 0, 0, 0, 1, 0 };
    CHECK_FALSE(stingNetCommand(read6, 6));

    CHECK(drainIpc(1).empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "IDENTIFY - magic, own locally administered MAC different from the W6100 one") {
    settings.mac[0] = 0x02; settings.mac[1] = 0x12; settings.mac[5] = 0x99;

    Reply r = shortCmd(RAWNET_CMD_IDENTIFY);
    REQUIRE(r.data.size() == 32);
    CHECK(r.status == 0);
    CHECK(memcmp(r.data.data(), "CERAWNET", 8) == 0);
    CHECK(r.data[9] == 2);                                  // protocol version

    std::vector<uint8_t> mac(r.data.begin() + 10, r.data.begin() + 16);
    CHECK((mac[0] & 0x03) == 0x02);                         // locally administered, unicast
    CHECK(memcmp(mac.data(), settings.mac, 6) != 0);
    CHECK(r.data[16] == 1);                                 // link
}

TEST_CASE_FIXTURE(FirmwareFixture, "CONFIG opens MACRAW and announces the IP, disable closes it") {
    icdCmd(RAWNET_CMD_CONFIG, 0);
    EthernetMacraw::resetFake();

    CHECK(configEnable().status == 0);
    CHECK(EthernetMacraw::open);
    REQUIRE(EthernetMacraw::sent.size() == 1);              // gratuitous ARP
    auto &g = EthernetMacraw::sent[0];
    CHECK(memcmp(g.data(), BCAST_MAC, 6) == 0);
    CHECK(g[12] == 0x08); CHECK(g[13] == 0x06);
    CHECK(memcmp(&g[14 + 14], STING_IP, 4) == 0);           // sender IP
    CHECK(memcmp(&g[14 + 24], STING_IP, 4) == 0);           // target IP

    CHECK(icdCmd(RAWNET_CMD_CONFIG, 0).status == 0);
    CHECK_FALSE(EthernetMacraw::open);
}

TEST_CASE_FIXTURE(FirmwareFixture, "CONFIG fails when MACRAW can't be opened") {
    icdCmd(RAWNET_CMD_CONFIG, 0);
    EthernetMacraw::resetFake();
    EthernetMacraw::beginResult = false;

    CHECK(configEnable().status == 0x04);
}

TEST_CASE_FIXTURE(FirmwareFixture, "parameter commands need the ICD10 form") {
    CHECK(shortCmd(RAWNET_CMD_RECV).status == 0x02);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ARP request for STinG's IP is answered with STinG's MAC") {
    netStart();
    auto mac = stingMac();

    uint8_t zero[6] = { 0 };
    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0806, arpPayload(1, PEER_MAC, PEER_IP, zero, STING_IP)));
    stingNetProcess();

    REQUIRE(EthernetMacraw::sent.size() == 1);
    auto &f = EthernetMacraw::sent[0];
    CHECK(memcmp(f.data(), PEER_MAC, 6) == 0);
    CHECK(memcmp(&f[6], mac.data(), 6) == 0);
    CHECK(f[14 + 7] == 2);                                  // reply
    CHECK(memcmp(&f[14 + 8], mac.data(), 6) == 0);
    CHECK(memcmp(&f[14 + 14], STING_IP, 4) == 0);
    CHECK(memcmp(&f[14 + 24], PEER_IP, 4) == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ARP request for another IP is ignored") {
    netStart();

    uint8_t zero[6] = { 0 };
    uint8_t otherIp[4] = { 192, 168, 1, 50 };
    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0806, arpPayload(1, PEER_MAC, PEER_IP, zero, otherIp)));
    stingNetProcess();

    CHECK(EthernetMacraw::sent.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "RECV delivers IP packets for STinG's MAC, padded, the rest is filtered out") {
    netStart();
    auto mac = stingMac();
    uint8_t otherMac[6] = { 0x02, 1, 2, 3, 4, 5 };

    auto pkt1 = ipPacket(PEER_IP, STING_IP, 31);            // odd length
    auto pkt2 = ipPacket(PEER_IP, STING_IP, 100);
    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, pkt1));
    EthernetMacraw::rxFrames.push_back(ethFrame(otherMac, PEER_MAC, 0x0800, pkt1));                         // other MAC
    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0800, ipPacket(PEER_IP, BCAST_MAC, 8)));   // IP broadcast, not wanted
    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x86dd, pkt1));                       // IPv6
    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, pkt2));
    stingNetProcess();

    Reply r = icdCmd(RAWNET_CMD_RECV, 4, 1);
    CHECK(r.status == 0);
    REQUIRE(r.data.size() >= 16);
    CHECK(r.data.size() % 16 == 0);
    CHECK(r.data[0] == 0xCE);
    CHECK(r.data[1] == 1);                                  // batch number

    uint16_t dataBytes = (r.data[2] << 8) | r.data[3];
    uint16_t pkts      = (r.data[4] << 8) | r.data[5];
    CHECK(pkts == 2);
    CHECK(dataBytes == (2 + 52) + (2 + 120));               // 51 B packet padded to 52
    CHECK(((r.data[6] << 8) | r.data[7]) == 0);             // nothing left

    size_t pos = 16;
    uint16_t len1 = (r.data[pos] << 8) | r.data[pos + 1];
    REQUIRE(len1 == pkt1.size());
    CHECK(std::vector<uint8_t>(r.data.begin() + pos + 2, r.data.begin() + pos + 2 + len1) == pkt1);
    pos += 2 + len1 + (len1 & 1);
    uint16_t len2 = (r.data[pos] << 8) | r.data[pos + 1];
    REQUIRE(len2 == pkt2.size());
    CHECK(std::vector<uint8_t>(r.data.begin() + pos + 2, r.data.begin() + pos + 2 + len2) == pkt2);
}

TEST_CASE_FIXTURE(FirmwareFixture, "IP broadcasts are delivered when enabled in CONFIG") {
    netStart(0x03);

    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0800, ipPacket(PEER_IP, BCAST_MAC, 8)));
    stingNetProcess();

    Reply r = icdCmd(RAWNET_CMD_RECV, 4, 1);
    CHECK(r.status == 0);
    CHECK(((r.data[4] << 8) | r.data[5]) == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "ethernet padding of short frames is not delivered") {
    netStart();
    auto mac = stingMac();

    auto pkt = ipPacket(PEER_IP, STING_IP, 6);              // 26 B packet in 60 B frame
    auto frame = ethFrame(mac.data(), PEER_MAC, 0x0800, pkt);
    frame.resize(60, 0);
    EthernetMacraw::rxFrames.push_back(frame);
    stingNetProcess();

    Reply r = icdCmd(RAWNET_CMD_RECV, 4, 1);
    CHECK(((r.data[16] << 8) | r.data[17]) == 26);
}

TEST_CASE_FIXTURE(FirmwareFixture, "RECV with the same batch number repeats the batch, new number gives the next one") {
    netStart();
    auto mac = stingMac();

    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10)));
    stingNetProcess();

    Reply first = icdCmd(RAWNET_CMD_RECV, 4, 5);
    Reply again = icdCmd(RAWNET_CMD_RECV, 4, 5);            // e.g. the DMA transfer failed on ST
    CHECK(first.status == 0);
    CHECK(again.data == first.data);

    Reply next = icdCmd(RAWNET_CMD_RECV, 4, 6);
    CHECK(next.status == 1);                                // empty
    CHECK(next.data.size() == 16);
    CHECK(((next.data[4] << 8) | next.data[5]) == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "RECV fits the batch into the ST's buffer, the rest stays for the next one") {
    netStart();
    auto mac = stingMac();

    for (int i = 0; i < 3; i++) {
        EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 400)));
    }
    stingNetProcess();

    Reply r = icdCmd(RAWNET_CMD_RECV, 2, 1);                // 1024 B - header + 2 packets of 422 B
    CHECK(r.data.size() <= 1024);
    CHECK(((r.data[4] << 8) | r.data[5]) == 2);
    CHECK(((r.data[6] << 8) | r.data[7]) == 1);             // one still waiting

    Reply r2 = icdCmd(RAWNET_CMD_RECV, 2, 2);
    CHECK(((r2.data[4] << 8) | r2.data[5]) == 1);
}

TEST_CASE_FIXTURE(FirmwareFixture, "SEND to unknown next hop asks ARP, sends the packet on the reply") {
    netStart();
    auto mac = stingMac();

    auto pkt = ipPacket(STING_IP, PEER_IP, 33);
    Reply r = doSend(sendEntry(PEER_IP, pkt), 1);
    CHECK((r.status & 0x7f) == 0);

    REQUIRE(EthernetMacraw::sent.size() == 1);              // ARP request only
    auto &req = EthernetMacraw::sent[0];
    CHECK(memcmp(req.data(), BCAST_MAC, 6) == 0);
    CHECK(req[14 + 7] == 1);
    CHECK(memcmp(&req[14 + 24], PEER_IP, 4) == 0);

    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0806, arpPayload(2, PEER_MAC, PEER_IP, mac.data(), STING_IP)));
    stingNetProcess();

    REQUIRE(EthernetMacraw::sent.size() == 2);
    auto &f = EthernetMacraw::sent[1];
    CHECK(memcmp(f.data(), PEER_MAC, 6) == 0);
    CHECK(memcmp(&f[6], mac.data(), 6) == 0);
    CHECK(f[12] == 0x08); CHECK(f[13] == 0x00);
    CHECK(std::vector<uint8_t>(f.begin() + 14, f.end()) == pkt);

    // MAC known now - the next packet goes out right away
    Reply r2 = doSend(sendEntry(PEER_IP, pkt), 2);
    CHECK(EthernetMacraw::sent.size() == 3);
}

TEST_CASE_FIXTURE(FirmwareFixture, "SEND with several packets, repeated batch number is not sent again") {
    netStart();
    auto mac = stingMac();
    uint8_t zero[6] = { 0 };

    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0806, arpPayload(1, PEER_MAC, PEER_IP, zero, STING_IP)));
    stingNetProcess();                                      // learns the peer from its request
    EthernetMacraw::sent.clear();

    auto b = sendEntry(PEER_IP, ipPacket(STING_IP, PEER_IP, 21));
    auto b2 = sendEntry(PEER_IP, ipPacket(STING_IP, PEER_IP, 40));
    b.insert(b.end(), b2.begin(), b2.end());

    doSend(b, 7);
    CHECK(EthernetMacraw::sent.size() == 2);

    doSend(b, 7);                                           // retry of a transfer that went through already
    CHECK(EthernetMacraw::sent.size() == 2);
}

TEST_CASE_FIXTURE(FirmwareFixture, "SEND status tells about waiting received packets") {
    netStart();
    auto mac = stingMac();

    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10)));
    stingNetProcess();

    Reply r = doSend(sendEntry(PEER_IP, ipPacket(STING_IP, PEER_IP, 10)), 1);
    CHECK((r.status & 0x80) == 0x80);
}

TEST_CASE_FIXTURE(FirmwareFixture, "SEND with bad length is refused without data phase") {
    netStart();

    Reply r = icdCmd(RAWNET_CMD_SEND, 0, 0, 0, 1, 0);
    CHECK(r.status == 0x02);
    CHECK(r.writeCount == 0);
    CHECK_FALSE(stingNetOwnsWrite());
}

TEST_CASE_FIXTURE(FirmwareFixture, "a new command ends a SEND data phase that didn't finish") {
    netStart();

    icdCmd(RAWNET_CMD_SEND, 0, 0x01, 0x00, 1, 1);           // 256 B announced, data never comes
    CHECK(stingNetOwnsWrite());

    uint8_t read6[6] = { 0x08, 0, 0, 0, 1, 0 };
    CHECK_FALSE(stingNetCommand(read6, 6));                 // host command - the next write data are host's again
    CHECK_FALSE(stingNetOwnsWrite());
}

//--------------------------------------------------------
// frame mode (RAWNET_CFG_FRAMES, MiNTnet): whole ethernet frames, ARP is the business of the stack on ST

static const uint8_t CFG_FRAMES_ON = RAWNET_CFG_ENABLE | RAWNET_CFG_FRAMES;

// SEND batch in frame mode: [len16][frame], padded to even
static std::vector<uint8_t> frameEntry(const std::vector<uint8_t> &frame)
{
    std::vector<uint8_t> e = { (uint8_t)(frame.size() >> 8), (uint8_t) frame.size() };
    e.insert(e.end(), frame.begin(), frame.end());
    if (e.size() & 1) e.push_back(0);
    return e;
}

// the frames of a RECV reply
static std::vector<std::vector<uint8_t>> recvFrames(const Reply &r)
{
    std::vector<std::vector<uint8_t>> frames;
    REQUIRE(r.data.size() >= 16);

    size_t pos = 16;
    size_t end = 16 + ((r.data[2] << 8) | r.data[3]);
    uint16_t count = (r.data[4] << 8) | r.data[5];

    for (int i = 0; i < count; i++) {
        REQUIRE(pos + 2 <= end);
        uint16_t len = (r.data[pos] << 8) | r.data[pos + 1];
        REQUIRE(pos + 2 + len <= end);
        frames.emplace_back(r.data.begin() + pos + 2, r.data.begin() + pos + 2 + len);
        pos += 2 + len + (len & 1);
    }

    CHECK(pos == end);
    return frames;
}

TEST_CASE_FIXTURE(FirmwareFixture, "frame mode - no gratuitous ARP, IDENTIFY tells the mode") {
    icdCmd(RAWNET_CMD_CONFIG, 0);
    EthernetMacraw::resetFake();

    CHECK(icdCmd(RAWNET_CMD_CONFIG, CFG_FRAMES_ON).status == 0);    // no IP - the stack on ST knows it
    CHECK(EthernetMacraw::open);
    CHECK(EthernetMacraw::sent.empty());

    CHECK((shortCmd(RAWNET_CMD_IDENTIFY).data[17] & 0x04) == 0x04);

    icdCmd(RAWNET_CMD_CONFIG, 0);
    CHECK((shortCmd(RAWNET_CMD_IDENTIFY).data[17] & 0x04) == 0);
}

TEST_CASE_FIXTURE(FirmwareFixture, "frame mode - RECV gives whole frames for our MAC + broadcasts, ARP is not answered") {
    netStart(CFG_FRAMES_ON);
    auto mac = stingMac();
    uint8_t otherMac[6]  = { 0x02, 1, 2, 3, 4, 5 };
    uint8_t multicast[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
    uint8_t zero[6] = { 0 };

    auto arp   = ethFrame(BCAST_MAC, PEER_MAC, 0x0806, arpPayload(1, PEER_MAC, PEER_IP, zero, STING_IP));
    auto ip    = ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 31));
    auto ipv6  = ethFrame(mac.data(), PEER_MAC, 0x86dd, std::vector<uint8_t>(40, 0x66));
    auto ipBc  = ethFrame(BCAST_MAC, PEER_MAC, 0x0800, ipPacket(PEER_IP, BCAST_MAC, 8));

    EthernetMacraw::rxFrames.push_back(arp);
    EthernetMacraw::rxFrames.push_back(ethFrame(otherMac, PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10)));   // other MAC
    EthernetMacraw::rxFrames.push_back(ip);
    EthernetMacraw::rxFrames.push_back(ethFrame(multicast, PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10)));  // multicast
    EthernetMacraw::rxFrames.push_back(ipv6);
    EthernetMacraw::rxFrames.push_back(ipBc);
    stingNetProcess();

    CHECK(EthernetMacraw::sent.empty());                    // no ARP reply from pico

    Reply r = icdCmd(RAWNET_CMD_RECV, 4, 1);
    CHECK(r.status == 0);
    CHECK(r.data.size() % 16 == 0);

    auto frames = recvFrames(r);
    REQUIRE(frames.size() == 4);
    CHECK(frames[0] == arp);
    CHECK(frames[1] == ip);
    CHECK(frames[2] == ipv6);
    CHECK(frames[3] == ipBc);
}

TEST_CASE_FIXTURE(FirmwareFixture, "frame mode - short frames stay as they came, the biggest frame fits") {
    netStart(CFG_FRAMES_ON);
    auto mac = stingMac();

    auto small = ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 6));
    small.resize(60, 0);                                    // ethernet padding - the stack on ST cuts it off
    auto big = ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 1480));
    REQUIRE(big.size() == 1514);

    EthernetMacraw::rxFrames.push_back(small);
    EthernetMacraw::rxFrames.push_back(big);
    stingNetProcess();

    auto frames = recvFrames(icdCmd(RAWNET_CMD_RECV, 16, 1));
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == small);
    CHECK(frames[1] == big);
}

TEST_CASE_FIXTURE(FirmwareFixture, "frame mode - SEND puts the frames on the wire as they are, without ARP") {
    netStart(CFG_FRAMES_ON);
    auto mac = stingMac();
    uint8_t zero[6] = { 0 };

    auto arp = ethFrame(BCAST_MAC, mac.data(), 0x0806, arpPayload(1, mac.data(), STING_IP, zero, PEER_IP));     // 42 B, even
    auto ip  = ethFrame(PEER_MAC, mac.data(), 0x0800, ipPacket(STING_IP, PEER_IP, 31));                         // odd

    auto b  = frameEntry(ip);
    auto b2 = frameEntry(arp);
    b.insert(b.end(), b2.begin(), b2.end());

    CHECK((doSend(b, 1).status & 0x7f) == 0);

    REQUIRE(EthernetMacraw::sent.size() == 2);
    CHECK(EthernetMacraw::sent[0] == ip);
    CHECK(EthernetMacraw::sent[1] == arp);

    doSend(b, 1);                                           // retry of a transfer that went through already
    CHECK(EthernetMacraw::sent.size() == 2);

    doSend(b, 2);
    CHECK(EthernetMacraw::sent.size() == 4);
}

TEST_CASE_FIXTURE(FirmwareFixture, "frame mode - SEND with bad frame length") {
    netStart(CFG_FRAMES_ON);

    std::vector<uint8_t> tooShort = { 0, 13 };              // smaller than ethernet header
    tooShort.resize(16, 0);
    CHECK((doSend(tooShort, 1).status & 0x7f) == 0x02);

    std::vector<uint8_t> tooBig = { 0x05, 0xeb };           // 1515 B
    tooBig.resize(1518, 0);
    CHECK((doSend(tooBig, 2).status & 0x7f) == 0x02);

    std::vector<uint8_t> overrun = { 0, 100 };              // more than the batch has
    overrun.resize(40, 0);
    CHECK((doSend(overrun, 3).status & 0x7f) == 0x02);

    CHECK(EthernetMacraw::sent.empty());
}

TEST_CASE_FIXTURE(FirmwareFixture, "changing the mode drops what was queued in the other format") {
    netStart();                                             // IP mode
    auto mac = stingMac();

    EthernetMacraw::rxFrames.push_back(ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10)));
    stingNetProcess();

    CHECK(icdCmd(RAWNET_CMD_CONFIG, CFG_FRAMES_ON).status == 0);
    CHECK(icdCmd(RAWNET_CMD_RECV, 4, 1).status == 1);       // empty

    // and frame mode is really on, without closing the socket in between
    CHECK(EthernetMacraw::open);
    auto frame = ethFrame(mac.data(), PEER_MAC, 0x0800, ipPacket(PEER_IP, STING_IP, 10));
    EthernetMacraw::rxFrames.push_back(frame);
    stingNetProcess();
    auto frames = recvFrames(icdCmd(RAWNET_CMD_RECV, 4, 2));
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == frame);
}

TEST_CASE_FIXTURE(FirmwareFixture, "IP mode works as before after frame mode was used") {
    netStart(CFG_FRAMES_ON);
    netStart();
    auto mac = stingMac();

    uint8_t zero[6] = { 0 };
    EthernetMacraw::rxFrames.push_back(ethFrame(BCAST_MAC, PEER_MAC, 0x0806, arpPayload(1, PEER_MAC, PEER_IP, zero, STING_IP)));
    stingNetProcess();

    REQUIRE(EthernetMacraw::sent.size() == 1);              // pico answers ARP again
    CHECK(EthernetMacraw::sent[0][14 + 7] == 2);
    CHECK(icdCmd(RAWNET_CMD_RECV, 4, 1).status == 1);       // and ARP doesn't go to ST
}
