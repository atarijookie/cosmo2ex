#include <Ethernet.h>

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "sting_net.h"

// Network for STinG over raw ethernet frames (W6100 socket 0 in MACRAW mode). STinG gets its own MAC address (derived
// from ours), the W6100 sockets for host connection keep using our MAC + IP. The pico filters received frames (only IPv4
// for STinG's MAC goes to ST), answers ARP for STinG's IP and resolves next hop MAC addresses for the IP packets the ST
// sends - so the ST sees just IP packets.
// In frame mode (RAWNET_CFG_FRAMES, for MiNTnet) the ST gets and sends whole ethernet frames, the stack there does ARP - the
// pico only keeps the frames for this MAC + broadcasts. Protocol description: ce_sting_stx_driver/README.md

#define RAWNET_PROTO_VERSION    2

#define RX_RING_SIZE        32768       // received IP packets waiting for RECV, as [len16][packet]
#define BATCH_MAX           16384       // max packet data in one RECV / SEND (RECV header not included)
#define RECV_HDR_SIZE       16
#define PENDING_COUNT       8           // IP packets waiting for ARP reply
#define ARP_CACHE_SIZE      16
#define ARP_ENTRY_TTL_MS    (10UL * 60UL * 1000UL)
#define ARP_RETRY_MS        1000
#define ARP_TRIES           3
#define FRAMES_PER_PROCESS  8           // max received frames handled in one stingNetProcess() call, so the main loop goes on

#define ETH_HDR_SIZE        14
#define ETH_TYPE_IP         0x0800
#define ETH_TYPE_ARP        0x0806
#define ARP_SIZE            28
#define IP_HDR_SIZE         20
#define IP_MAX_SIZE         1500
#define FRAME_MAX_SIZE      (ETH_HDR_SIZE + IP_MAX_SIZE)

// status byte of the commands
#define ST_OK               0x00
#define ST_EMPTY            0x01        // RECV: no packet in this batch
#define ST_BAD_PARAM        0x02
#define ST_NOT_ENABLED      0x03        // SEND before CONFIG enabled the network
#define ST_NO_MACRAW        0x04        // CONFIG: couldn't open the MACRAW socket
#define ST_RX_PENDING       0x80        // SEND: received packets are waiting, do RECV

static EthernetMacraw macraw;

static bool     enabled;
static bool     acceptBroadcast;
static bool     frameMode;
static uint8_t  stingMac[6];
static uint8_t  stingIp[4];

static const uint8_t bcastMac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t zeroMac[6]  = { 0, 0, 0, 0, 0, 0 };

static struct {
    uint32_t rxFrames;          // frames read from W6100 (all, also the ones not for STinG)
    uint32_t rxDelivered;       // IP packets (frames in frame mode) given to ST
    uint32_t rxDropped;         // the same dropped - ring full or too big for ST's RECV
    uint32_t txPackets;         // IP packets (frames in frame mode) sent
    uint32_t txDropped;         // the same dropped - no ARP reply, no pending slot, frame not sent
    uint32_t arpRequests;       // ARP requests sent
    uint32_t arpReplies;        // ARP replies sent
    uint32_t badCommands;       // commands with bad parameters
} stats;

//------------------
// received IP packets (frames in frame mode), each stored as [len16][packet]
static uint8_t  rxRing[RX_RING_SIZE];
static uint32_t ringHead, ringTail, ringUsed, ringCount;

// the last RECV batch - kept until RECV with other batch number comes, so a failed transfer can be repeated
static uint8_t  batch[RECV_HDR_SIZE + BATCH_MAX + 16];
static uint32_t batchSize;
static uint16_t batchPkts;
static uint8_t  batchNo;
static bool     batchValid;

// data of the current SEND
static uint8_t  sendBuf[BATCH_MAX];
static uint32_t sendExpected, sendGot;
static bool     writeOwned;
static uint8_t  sendBatchNo, sendLastNo;
static bool     sendHaveLast;

static uint8_t  frameBuf[ETH_HDR_SIZE + IP_MAX_SIZE];

struct ArpEntry {
    bool     valid;
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint32_t time;
};
static ArpEntry arpCache[ARP_CACHE_SIZE];

struct Pending {
    bool     used;
    uint8_t  ip[4];             // next hop waiting for ARP reply
    uint32_t lastTry;
    uint8_t  tries;
    uint16_t len;
    uint8_t  data[IP_MAX_SIZE];
};
static Pending pending[PENDING_COUNT];

//------------------
static void ringPut(const uint8_t *data, uint32_t len)
{
    while(len > 0) {
        uint32_t now = MIN(len, RX_RING_SIZE - ringHead);
        memcpy(rxRing + ringHead, data, now);
        ringHead = (ringHead + now) % RX_RING_SIZE;
        data += now;
        len -= now;
    }
}

static void ringGet(uint8_t *data, uint32_t len)        // data == NULL just drops the bytes
{
    ringUsed -= len;

    while(len > 0) {
        uint32_t now = MIN(len, RX_RING_SIZE - ringTail);
        if(data) {
            memcpy(data, rxRing + ringTail, now);
            data += now;
        }
        ringTail = (ringTail + now) % RX_RING_SIZE;
        len -= now;
    }
}

static uint16_t ringPeekLen(void)
{
    return (rxRing[ringTail] << 8) | rxRing[(ringTail + 1) % RX_RING_SIZE];
}

static void ringPush(const uint8_t *data, uint16_t len)
{
    if(RX_RING_SIZE - ringUsed < (uint32_t) (2 + len)) {      // ST doesn't fetch them fast enough
        stats.rxDropped++;
        return;
    }

    uint8_t lenBytes[2];
    storeWord(lenBytes, len);
    ringPut(lenBytes, 2);
    ringPut(data, len);
    ringUsed += 2 + len;
    ringCount++;
}

static void resetQueues(void)
{
    ringHead = ringTail = ringUsed = ringCount = 0;
    batchValid = false;
    sendHaveLast = false;
    writeOwned = false;
    memset(pending, 0, sizeof(pending));
    memset(arpCache, 0, sizeof(arpCache));
}

//------------------
static void replyStatus(uint8_t status)
{
    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(!bfr) {
        debug("stingNet - replyStatus - ipcGetFreeBuffer failed\n");
        return;
    }
    ipcSetBufferAndPutToFifo(bfr, 1, STATE_READ_STATUS, 1, &status, 1);
}

static bool replyData(const uint8_t *data, uint32_t len)
{
    while(len > 0) {
        IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
        if(!bfr) {
            debug("stingNet - replyData - ipcGetFreeBuffer failed\n");
            return false;
        }

        uint32_t now = MIN(len, (uint32_t) IPC_BUFFER_SIZE);
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_READ, now, (uint8_t *) data, now);
        data += now;
        len -= now;
    }
    return true;
}

//------------------
static void sendArp(uint16_t op, const uint8_t *dstMac, const uint8_t *tha, const uint8_t *tpa)
{
    uint8_t f[ETH_HDR_SIZE + ARP_SIZE];

    memcpy(f, dstMac, 6);
    memcpy(f + 6, stingMac, 6);
    storeWord(f + 12, ETH_TYPE_ARP);

    uint8_t *a = f + ETH_HDR_SIZE;
    storeWord(a, 1);                // ethernet
    storeWord(a + 2, ETH_TYPE_IP);  // IPv4
    a[4] = 6;
    a[5] = 4;
    storeWord(a + 6, op);
    memcpy(a + 8,  stingMac, 6);    // sender
    memcpy(a + 14, stingIp, 4);
    memcpy(a + 18, tha, 6);         // target
    memcpy(a + 24, tpa, 4);

    macraw.sendFrame(f, sizeof(f), NULL, 0);    // EthernetMacraw logs why, if it fails
}

static void sendArpRequest(const uint8_t *ip)
{
    stats.arpRequests++;
    sendArp(1, bcastMac, zeroMac, ip);
}

static bool sendIpFrame(const uint8_t *dstMac, const uint8_t *data, uint16_t len)
{
    uint8_t eth[ETH_HDR_SIZE];
    memcpy(eth, dstMac, 6);
    memcpy(eth + 6, stingMac, 6);
    storeWord(eth + 12, ETH_TYPE_IP);

    if(!macraw.sendFrame(eth, ETH_HDR_SIZE, data, len)) {
        stats.txDropped++;
        return false;
    }

    stats.txPackets++;
    return true;
}

static const uint8_t *arpLookup(const uint8_t *ip)
{
    uint32_t now = millis();

    for(int i=0; i<ARP_CACHE_SIZE; i++) {
        if(!arpCache[i].valid || memcmp(arpCache[i].ip, ip, 4) != 0) {
            continue;
        }

        if((now - arpCache[i].time) > ARP_ENTRY_TTL_MS) {     // too old, ask again
            arpCache[i].valid = false;
            return NULL;
        }

        return arpCache[i].mac;
    }

    return NULL;
}

static void arpStore(const uint8_t *ip, const uint8_t *mac)
{
    int slot = -1;

    for(int i=0; i<ARP_CACHE_SIZE && slot < 0; i++) {      // existing entry for this IP
        if(arpCache[i].valid && memcmp(arpCache[i].ip, ip, 4) == 0) {
            slot = i;
        }
    }

    for(int i=0; i<ARP_CACHE_SIZE && slot < 0; i++) {      // free entry
        if(!arpCache[i].valid) {
            slot = i;
        }
    }

    if(slot < 0) {                                          // the oldest entry
        slot = 0;
        for(int i=1; i<ARP_CACHE_SIZE; i++) {
            if((int32_t) (arpCache[i].time - arpCache[slot].time) < 0) {
                slot = i;
            }
        }
    }

    arpCache[slot].valid = true;
    memcpy(arpCache[slot].ip, ip, 4);
    memcpy(arpCache[slot].mac, mac, 6);
    arpCache[slot].time = millis();

    for(int i=0; i<PENDING_COUNT; i++) {            // send what was waiting for this MAC
        if(pending[i].used && memcmp(pending[i].ip, ip, 4) == 0) {
            sendIpFrame(mac, pending[i].data, pending[i].len);
            pending[i].used = false;
        }
    }
}

static bool isPendingFor(const uint8_t *ip)
{
    for(int i=0; i<PENDING_COUNT; i++) {
        if(pending[i].used && memcmp(pending[i].ip, ip, 4) == 0) {
            return true;
        }
    }
    return false;
}

static void transmitIp(const uint8_t *nextHop, const uint8_t *data, uint16_t len)
{
    const uint8_t *mac = (memcmp(nextHop, bcastMac, 4) == 0) ? bcastMac : arpLookup(nextHop);

    if(mac) {
        sendIpFrame(mac, data, len);
        return;
    }

    // MAC not known yet - keep the packet until ARP reply arrives
    int slot = -1;
    for(int i=0; i<PENDING_COUNT; i++) {
        if(!pending[i].used) {
            slot = i;
            break;
        }
    }

    if(slot < 0) {                                  // all slots wait for ARP, drop it, TCP will send it again
        stats.txDropped++;
        return;
    }

    bool askNow = !isPendingFor(nextHop);           // one ARP request per next hop is enough

    Pending *p = &pending[slot];
    p->used = true;
    memcpy(p->ip, nextHop, 4);
    memcpy(p->data, data, len);
    p->len = len;
    p->tries = 1;
    p->lastTry = millis();

    if(askNow) {
        sendArpRequest(nextHop);
    }
}

static void handleArp(const uint8_t *a)
{
    if(getWord((uint8_t *) a) != 1 || getWord((uint8_t *) a + 2) != ETH_TYPE_IP || a[4] != 6 || a[5] != 4) {
        return;
    }

    uint16_t op = getWord((uint8_t *) a + 6);
    const uint8_t *sha = a + 8;
    const uint8_t *spa = a + 14;
    const uint8_t *tpa = a + 24;

    static const uint8_t zeroIp[4] = { 0, 0, 0, 0 };
    bool haveIp = memcmp(stingIp, zeroIp, 4) != 0;
    bool forUs  = haveIp && memcmp(tpa, stingIp, 4) == 0;

    if(memcmp(spa, zeroIp, 4) != 0 && (forUs || isPendingFor(spa) || arpLookup(spa))) {     // learn the sender (not from ARP probes)
        arpStore(spa, sha);
    }

    if(op == 1 && forUs) {                          // who has STinG's IP? we do
        stats.arpReplies++;
        sendArp(2, sha, sha, spa);
    }
}

static void arpRetries(void)
{
    uint32_t now = millis();

    for(int i=0; i<PENDING_COUNT; i++) {
        Pending *p = &pending[i];
        if(!p->used || (now - p->lastTry) < ARP_RETRY_MS) {
            continue;
        }

        if(p->tries >= ARP_TRIES) {                 // nobody answers, drop all waiting for this IP
            for(int j=0; j<PENDING_COUNT; j++) {
                if(pending[j].used && memcmp(pending[j].ip, p->ip, 4) == 0) {
                    pending[j].used = false;
                    stats.txDropped++;
                }
            }
            continue;
        }

        for(int j=0; j<PENDING_COUNT; j++) {        // the others for this IP don't need their own request
            if(pending[j].used && memcmp(pending[j].ip, p->ip, 4) == 0) {
                pending[j].tries++;
                pending[j].lastTry = now;
            }
        }
        sendArpRequest(p->ip);
    }
}

//------------------
static void onIdentify(void)
{
    uint8_t r[32];
    memset(r, 0, sizeof(r));

    memcpy(r, "CERAWNET", 8);
    storeWord(r + 8, RAWNET_PROTO_VERSION);
    memcpy(r + 10, stingMac, 6);
    r[16] = (Ethernet.linkStatus() == LinkON) ? 1 : 0;
    r[17] = (enabled ? 0x01 : 0) | (macraw.isOpen() ? 0x02 : 0) | (frameMode ? 0x04 : 0);
    storeWord(r + 18, BATCH_MAX);

    IPAddress ip = Ethernet.localIP();              // pico's own IP, just for info
    for(int i=0; i<4; i++) {
        r[20 + i] = ip[i];
    }

    replyData(r, sizeof(r));
    replyStatus(ST_OK);
}

static void onConfig(const uint8_t *par)
{
    uint8_t flags = par[0];

    if(!(flags & RAWNET_CFG_ENABLE)) {              // disable
        debug("stingNet - disabled\n");
        enabled = false;
        frameMode = false;
        macraw.stop();
        resetQueues();
        replyStatus(ST_OK);
        return;
    }

    bool wantFrames = (flags & RAWNET_CFG_FRAMES) != 0;

    if(!enabled || wantFrames != frameMode) {       // was off or other mode (ring holds the other format), start clean
        resetQueues();
    }

    frameMode = wantFrames;

    bool ipChanged = !enabled || memcmp(stingIp, par + 1, 4) != 0;

    if(enabled && ipChanged && !frameMode) {
        debug("stingNet - IP changed to %d.%d.%d.%d\n", par[1], par[2], par[3], par[4]);
    }

    memcpy(stingIp, par + 1, 4);
    acceptBroadcast = (flags & RAWNET_CFG_BROADCAST) != 0;

    if(!macraw.isOpen() && !macraw.begin(false)) {  // no MAC filter - STinG has its own MAC, we filter in stingNetProcess()
        enabled = false;
        replyStatus(ST_NO_MACRAW);
        return;
    }

    if(!enabled) {
        if(frameMode) {
            debug("stingNet - enabled, frame mode, MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                  stingMac[0], stingMac[1], stingMac[2], stingMac[3], stingMac[4], stingMac[5]);
        } else {
            debug("stingNet - enabled, IP %d.%d.%d.%d, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", stingIp[0], stingIp[1], stingIp[2], stingIp[3],
                  stingMac[0], stingMac[1], stingMac[2], stingMac[3], stingMac[4], stingMac[5]);
        }
    }

    enabled = true;

    static const uint8_t noIp[4] = { 0xff, 0xff, 0xff, 0xff };     // STinG's port without IP set yet
    if(!frameMode && ipChanged && memcmp(stingIp, noIp, 4) != 0 && (stingIp[0] | stingIp[1] | stingIp[2] | stingIp[3]) != 0) {
        sendArp(1, bcastMac, zeroMac, stingIp);     // gratuitous ARP - tell the network where STinG's IP is now
    }
    replyStatus(ST_OK);
}

static void onSend(const uint8_t *par)
{
    uint32_t count = get24bits((uint8_t *) par);

    if(count == 0 || count > BATCH_MAX) {
        stats.badCommands++;
        replyStatus(ST_BAD_PARAM);
        return;
    }

    sendExpected = count;
    sendGot = 0;
    sendBatchNo = par[3];
    writeOwned = true;

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);   // let core 1 get the data from ST, they come to stingNetWriteData()
    if(!bfr) {
        debug("stingNet - onSend - ipcGetFreeBuffer failed\n");
        writeOwned = false;
        return;
    }
    ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_WRITE, count, NULL, 0);
}

// frame mode: [len16][ethernet frame], padded to even length
static uint8_t transmitFrames(void)
{
    uint32_t off = 0;

    while(off + 2 <= sendExpected) {
        uint16_t len = getWord(sendBuf + off);

        if(len == 0) {
            break;
        }

        if(len < ETH_HDR_SIZE || len > FRAME_MAX_SIZE || off + 2 + len > sendExpected) {
            stats.badCommands++;
            return ST_BAD_PARAM;
        }

        if(macraw.sendFrame(sendBuf + off + 2, len, NULL, 0)) {
            stats.txPackets++;
        } else {
            stats.txDropped++;
        }

        off += 2 + len;
        off += (off & 1);
    }

    return ST_OK;
}

static uint8_t transmitBatch(void)
{
    if(frameMode) {
        return transmitFrames();
    }

    uint32_t off = 0;

    while(off + 6 <= sendExpected) {                // [len16][next hop IP][IP packet], padded to even length
        uint16_t len = getWord(sendBuf + off);

        if(len == 0) {
            break;
        }

        if(len < IP_HDR_SIZE || len > IP_MAX_SIZE || off + 6 + len > sendExpected) {
            stats.badCommands++;
            return ST_BAD_PARAM;
        }

        transmitIp(sendBuf + off + 2, sendBuf + off + 6, len);

        off += 6 + len;
        off += (off & 1);
    }

    return ST_OK;
}

static void onRecv(const uint8_t *par)
{
    uint8_t maxSectors = par[0];
    uint8_t no = par[1];

    if(maxSectors == 0) {
        stats.badCommands++;
        replyStatus(ST_BAD_PARAM);
        return;
    }

    if(!batchValid || no != batchNo) {              // new batch - the previous one arrived fine, build next one
        uint32_t room = MIN((uint32_t) maxSectors * 512, (uint32_t) (RECV_HDR_SIZE + BATCH_MAX));
        uint32_t pos = RECV_HDR_SIZE;
        uint16_t pkts = 0;

        while(ringCount > 0) {
            uint16_t len = ringPeekLen();
            uint32_t entry = 2 + len + (len & 1);

            if(pos + entry > room) {
                if(pkts == 0) {                     // would never fit into this ST's buffer, drop it
                    ringGet(NULL, 2 + len);
                    ringCount--;
                    stats.rxDropped++;
                    continue;
                }
                break;
            }

            ringGet(batch + pos, 2 + len);
            if(len & 1) {
                batch[pos + 2 + len] = 0;
            }
            ringCount--;
            pos += entry;
            pkts++;
        }

        uint32_t dataBytes = pos - RECV_HDR_SIZE;
        while(pos & 15) {                           // last 1..15 bytes would stay in ST's DMA FIFO
            batch[pos++] = 0;
        }

        memset(batch, 0, RECV_HDR_SIZE);
        batch[0] = 0xCE;
        batch[1] = no;
        storeWord(batch + 2, dataBytes);
        storeWord(batch + 4, pkts);
        storeWord(batch + 6, MIN(ringCount, (uint32_t) 0xffff));
        batch[8] = (Ethernet.linkStatus() == LinkON) ? 1 : 0;

        batchSize = pos;
        batchPkts = pkts;
        batchNo = no;
        batchValid = true;
        stats.rxDelivered += pkts;
    }

    replyData(batch, batchSize);
    replyStatus(batchPkts ? ST_OK : ST_EMPTY);
}

static void onGetStats(void)
{
    uint8_t r[32];
    uint32_t *s = (uint32_t *) &stats;

    for(int i=0; i<8; i++) {
        storeDword(r + i*4, s[i]);
    }

    replyData(r, sizeof(r));
    replyStatus(ST_OK);
}

//------------------
bool stingNetCommand(uint8_t *cmd, uint32_t cmdLen)
{
    writeOwned = false;                             // any new command ends the previous DMA write, even if it failed half way

    bool icd = (cmd[0] & 0x1f) == 0x1f;
    uint8_t *p = icd ? cmd + 1 : cmd;

    if(cmdLen < (icd ? 7u : 6u)) {
        return false;
    }

    if((p[0] & 0x1f) != 0 || p[1] != 'C' || p[2] != 'E' || (p[3] & 0x7f) != HOSTMOD_RAW_NET) {     // not ours, for host
        return false;
    }

    if(stingMac[0] == 0) {                          // STinG's MAC - our MAC with one more bit flipped, still locally administered
        memcpy(stingMac, settings.mac, 6);
        stingMac[0] ^= 0x04;
    }

    uint8_t func = p[4];
    const uint8_t *par = p + 5;                     // SCSI(6): 1 param byte, ICD10: 5 param bytes
    bool long10 = icd && cmdLen >= 11;

    switch(func) {
        case RAWNET_CMD_IDENTIFY:   onIdentify(); return true;
        case RAWNET_CMD_GET_STATS:  onGetStats(); return true;
    }

    if(!long10) {                                   // the rest needs the parameters of ICD10
        stats.badCommands++;
        replyStatus(ST_BAD_PARAM);
        return true;
    }

    switch(func) {
        case RAWNET_CMD_CONFIG: onConfig(par); break;
        case RAWNET_CMD_SEND:   onSend(par);   break;
        case RAWNET_CMD_RECV:   onRecv(par);   break;

        default:
            stats.badCommands++;
            replyStatus(ST_BAD_PARAM);
            break;
    }

    return true;
}

bool stingNetOwnsWrite(void)
{
    return writeOwned;
}

void stingNetWriteData(uint8_t *data, uint32_t len)
{
    uint32_t now = MIN(len, sendExpected - sendGot);
    memcpy(sendBuf + sendGot, data, now);
    sendGot += now;

    if(sendGot < sendExpected) {
        return;
    }

    writeOwned = false;                             // got all of it

    uint8_t status;
    if(!enabled) {
        status = ST_NOT_ENABLED;
    } else if(sendHaveLast && sendBatchNo == sendLastNo) {  // repeated batch (retry after failed transfer) - sent already
        status = ST_OK;
    } else {
        status = transmitBatch();
    }

    sendLastNo = sendBatchNo;
    sendHaveLast = true;

    if(ringCount > 0) {
        status |= ST_RX_PENDING;
    }

    replyStatus(status);
}

void stingNetProcess(void)
{
    if(!macraw.isOpen()) {                          // STinG not active - nothing to do, no SPI traffic
        return;
    }

    for(int i=0; i<FRAMES_PER_PROCESS; i++) {
        int len = macraw.parseFrame();
        if(len <= 0) {
            break;
        }

        stats.rxFrames++;

        if(macraw.read(frameBuf, ETH_HDR_SIZE) != ETH_HDR_SIZE) {
            continue;
        }

        bool toUs  = memcmp(frameBuf, stingMac, 6) == 0;
        bool bcast = memcmp(frameBuf, bcastMac, 6) == 0;

        if(!toUs && !bcast) {                       // the rest of the frame is skipped by next parseFrame(), without SPI transfer
            continue;
        }

        if(frameMode) {                             // whole frame goes to ST, ARP too - the stack there handles it
            uint16_t rest = MIN((uint16_t) (len - ETH_HDR_SIZE), (uint16_t) IP_MAX_SIZE);
            int got = macraw.read(frameBuf + ETH_HDR_SIZE, rest);
            ringPush(frameBuf, ETH_HDR_SIZE + got);
            continue;
        }

        uint16_t type = getWord(frameBuf + 12);

        if(type == ETH_TYPE_ARP) {
            if(macraw.read(frameBuf + ETH_HDR_SIZE, ARP_SIZE) == ARP_SIZE) {
                handleArp(frameBuf + ETH_HDR_SIZE);
            }
            continue;
        }

        if(type != ETH_TYPE_IP || !enabled || (bcast && !acceptBroadcast)) {
            continue;
        }

        uint16_t ipLen = MIN((uint16_t) (len - ETH_HDR_SIZE), (uint16_t) IP_MAX_SIZE);
        int got = macraw.read(frameBuf + ETH_HDR_SIZE, ipLen);
        if(got < IP_HDR_SIZE) {
            continue;
        }

        uint16_t total = getWord(frameBuf + ETH_HDR_SIZE + 2);     // IP total length - short frames have ethernet padding after the packet
        if(total >= IP_HDR_SIZE && total < got) {
            got = total;
        }

        ringPush(frameBuf + ETH_HDR_SIZE, got);
    }

    macraw.skip();
    arpRetries();
}
