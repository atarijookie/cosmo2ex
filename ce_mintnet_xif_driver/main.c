// CE_ETH.XIF - MiNTnet interface driver for CosmosEx pico HDD (W6100 ethernet). See README.md.
//
// Moves whole ethernet frames between inet4.xdd and the pico (frame mode of HOSTMOD_RAW_NET) - the stack here does ARP.
// All the ACSI / SCSI traffic while running is done in one place, poll(), which runs as a root timeout: that's in kernel
// context with interrupts on, so hz_200 keeps counting for the timeouts of libacsiscsi. xifOutput() only queues the frame.

#include <stdint.h>

#include "acsi.h"
#include "stdlib.h"

#include "mint.h"
#include "cenet.h"

#define IF_NAME             "en"    // the unit number is added by if_getfreeunit(): en0, en1...

#define POLL_FAST_MS        20      // the kernel counts timeouts in 20 ms steps, less makes no sense
#define POLL_IDLE_MS        100
#define FAST_HOLD_TICKS     200     // poll fast for this long after last traffic, in 200 Hz ticks
#define RECV_ROUNDS         2       // max RECV commands in one poll - more if the pico says more packets are waiting
#define MAX_PACKETS         5       // 5 full segments fit W6100's 8 KB receive buffer of socket 0, see README.md

#define DMA_BUF_SIZE        (NET_BUF_SIZE + 16)     // + for alignment
#define DMA_ALLOC_SIZE      (2 * DMA_BUF_SIZE + 512 + 16)

struct kerinfo *KERNEL;
struct netinfo *NETINFO;

static struct netif ifEn;
static TNetInfo info;

static uint8 *rxBuf;                // both in ST RAM
static uint8 *txBuf;

static uint8 up;
static uint8 rxNo = 1;              // number of the RECV batch to ask for, the same again after a failed transfer
static uint8 txNo = 1;
static uint32 lastTraffic;          // hz_200 of last sent / received frame

static void *pollTimer;             // the running poll chain, NULL when not scheduled
static volatile uint8 kickPending;
static BUF *carryBuf;               // dequeued frame which didn't fit into the previous SEND

//--------------------------------------------------------------------------------

static uint16 getWord(const uint8 *p)
{
    return (p[0] << 8) | p[1];
}

static uint8 *alignDma(uint32 addr)
{
    return (uint8 *) ((addr + 15) & ~15UL);     // aligned for every DMA (TT SCSI needs longwords)
}

// Put as many queued frames as fit into one SEND. Returns TRUE if something was sent.
static uint8 sendQueued(void)
{
    uint32 pos = 0;
    uint8 pkts = 0;

    while(pkts < 255) {
        BUF *buf = carryBuf;

        if(buf) {
            carryBuf = NULL;
        } else {
            buf = if_dequeue(&ifEn.snd);
        }

        if(!buf) {
            break;
        }

        uint32 len = buf->dend - buf->dstart;
        uint32 entry = 2 + len + (len & 1);         // [len16][frame], padded to even

        if(pos + entry > NET_BUF_SIZE) {
            carryBuf = buf;                         // netinfo has no way to put it back into the queue
            break;
        }

        txBuf[pos]     = len >> 8;
        txBuf[pos + 1] = len;
        fastCopy(txBuf + pos + 2, buf->dstart, len);
        if(len & 1) {
            txBuf[pos + 2 + len] = 0;
        }

        pos += entry;
        pkts++;
        buf_deref(buf, BUF_NORMAL);
    }

    if(pkts == 0) {
        return FALSE;
    }

    if(netSend(txBuf, pos, txNo, pkts) < 0) {       // lost - TCP sends it again
        ifEn.out_errors += pkts;
    } else {
        ifEn.out_packets += pkts;
    }
    txNo++;

    return TRUE;
}

static void passFrame(const uint8 *frame, uint16 len)
{
    BUF *buf = buf_alloc(len + 100, 50, BUF_ATOMIC);
    if(!buf) {
        ifEn.in_errors++;
        return;
    }

    buf->dend = buf->dstart + len;
    fastCopy(buf->dstart, frame, len);

    if(ifEn.bpf) {
        bpf_input(&ifEn, buf);
    }

    if(if_input(&ifEn, buf, 0, eth_remove_hdr(buf)) == 0) {
        ifEn.in_packets++;
    } else {
        ifEn.in_errors++;
    }
}

// Get what's waiting on the pico. Returns TRUE if something came.
static uint8 receive(void)
{
    uint8 sectors = NET_BUF_SIZE >> 9;
    uint8 got = FALSE;

    for(int round = 0; round < RECV_ROUNDS; round++) {
        int status = netRecv(rxBuf, sectors, rxNo);

        if(status < 0 || rxBuf[0] != 0xCE || rxBuf[1] != rxNo) {    // failed - ask for the same batch next time
            break;
        }

        rxNo++;                                     // got it, the next RECV tells the pico to drop it

        uint16 pkts    = getWord(rxBuf + 4);
        uint16 waiting = getWord(rxBuf + 6);
        uint32 pos = RECV_HDR_SIZE;
        uint32 end = RECV_HDR_SIZE + getWord(rxBuf + 2);

        while(pkts-- > 0 && pos + 2 <= end) {
            uint16 len = getWord(rxBuf + pos);

            if(len < ETH_HLEN || pos + 2 + len > end) {
                ifEn.in_errors++;
                break;
            }

            passFrame(rxBuf + pos + 2, len);
            pos += 2 + len + (len & 1);
            got = TRUE;
        }

        if(waiting == 0) {
            break;
        }
    }

    return got;
}

static void poll(void *proc, long arg)
{
    pollTimer = NULL;

    if(!up) {
        return;
    }

    if(*FLOCK == 0) {                               // else somebody uses the DMA - try on next round, don't wait here
        uint8 traffic = sendQueued();
        traffic |= receive();

        if(traffic) {
            lastTraffic = *HZ_200;
        }
    }

    uint8 fast = (*HZ_200 - lastTraffic) < FAST_HOLD_TICKS;
    pollTimer = addroottimeout(fast ? POLL_FAST_MS : POLL_IDLE_MS, poll, 0);
}

// a frame got queued - don't wait for the (maybe slow) next poll
static void kick(void *proc, long arg)
{
    kickPending = FALSE;

    if(!up) {
        return;
    }

    lastTraffic = *HZ_200;

    if(pollTimer) {
        cancelroottimeout(pollTimer);
    }
    poll(proc, arg);
}

//--------------------------------------------------------------------------------
// driver functions

static long xifOpen(struct netif *nif)
{
    if(netConfig(RAWNET_CFG_ENABLE | RAWNET_CFG_FRAMES, 0) != RAWNET_ST_OK) {
        return ENODEV_;
    }

    rxNo = txNo = 1;
    up = TRUE;
    lastTraffic = *HZ_200;
    pollTimer = addroottimeout(POLL_FAST_MS, poll, 0);
    return 0;
}

static long xifClose(struct netif *nif)
{
    up = FALSE;

    if(pollTimer) {
        cancelroottimeout(pollTimer);
        pollTimer = NULL;
    }

    if(carryBuf) {
        buf_deref(carryBuf, BUF_NORMAL);
        carryBuf = NULL;
    }
    if_flushq(&nif->snd);

    netConfig(0, 0);
    return 0;
}

// called through pc_xif_output
long xifOutput(struct netif *nif, BUF *buf, const char *hwaddr, long hwlen, long pktype)
{
    BUF *nbuf = eth_build_hdr(buf, nif, hwaddr, pktype);    // buf isn't valid after this
    if(!nbuf) {
        nif->out_errors++;
        return ENOMEM_;
    }

    if(nif->bpf) {
        bpf_input(nif, nbuf);
    }

    if(!up) {
        buf_deref(nbuf, BUF_NORMAL);
        return ENODEV_;
    }

    int16 res = if_enqueue(&nif->snd, nbuf, nbuf->info);
    if(res) {
        nif->out_errors++;
        return res;
    }

    if(!kickPending) {
        kickPending = TRUE;
        if(!addroottimeout(0, kick, ROOT_TIMEOUT_FROM_INT)) {   // no free timeout? the running poll takes it then
            kickPending = FALSE;
        }
    }

    return 0;
}

// called through pc_xif_ioctl
long xifIoctl(struct netif *nif, long cmd, long arg)
{
    switch(cmd) {
        case SIOCSIFNETMASK:
        case SIOCSIFFLAGS:
        case SIOCSIFADDR:
            return 0;

        case SIOCSIFMTU:                            // MiNTnet has already set nif->mtu to the new value, only limit it
            if(nif->mtu > ETH_MAX_DLEN) {
                nif->mtu = ETH_MAX_DLEN;
            }
            return 0;
    }

    return ENOSYS_;                                 // SIOCSIFHWADDR too - the pico decides the MAC
}

//--------------------------------------------------------------------------------

static void printHexByte(uint8 val)
{
    static const char hex[] = "0123456789ABCDEF";
    char str[3] = { hex[val >> 4], hex[val & 15], 0 };
    c_conws(str);
}

static long fail(const char *why)
{
    c_conws("CE_ETH.XIF: ");
    c_conws(why);
    c_conws("\r\n");
    return 1;
}

// inet4.xdd calls this through startup.s at boot, in supervisor mode. Not 0 = the driver gets unloaded.
long xifInit(struct kerinfo *k, struct netinfo *n)
{
    KERNEL  = k;
    NETINFO = n;

    // This program's BSS comes from kmalloc(), which can be Fast RAM on TT / Falcon, where ACSI DMA can't reach.
    // kerinfo's dmabuf_alloc() is NULL in the kernel, so Mxalloc(). F_KEEP - it must stay after the loading process.
    long mem = m_xalloc(DMA_ALLOC_SIZE, MX_STONLY | MX_KEEP);
    if(mem <= 0) {
        return fail("no ST RAM for DMA buffers");
    }

    rxBuf = alignDma(mem);
    txBuf = alignDma((uint32) rxBuf + DMA_BUF_SIZE);
    netSetDmaBuffer(alignDma((uint32) txBuf + DMA_BUF_SIZE));

    const char *problem = NULL;

    if(!netFindDevice()) {
        problem = "CosmosEx not found";
    } else if(netIdentify(&info) < 0) {
        problem = "CosmosEx doesn't answer";
    } else if(info.version < RAWNET_VERSION_FRAMES) {
        problem = "CosmosEx firmware too old, update it";
    }

    if(problem) {
        m_free(mem);                                // F_KEEP - nobody else would free it
        return fail(problem);
    }

    strcpy(ifEn.name, IF_NAME);
    ifEn.unit           = if_getfreeunit(ifEn.name);
    ifEn.flags          = IFF_BROADCAST;
    ifEn.mtu            = ETH_MAX_DLEN;
    ifEn.hwtype         = HWTYPE_ETH;
    ifEn.hwlocal.len    = ETH_ALEN;
    ifEn.hwbrcst.len    = ETH_ALEN;
    memcpy(ifEn.hwlocal.bytes, info.mac, ETH_ALEN);
    memset(ifEn.hwbrcst.bytes, 0xff, ETH_ALEN);
    ifEn.snd.maxqlen    = IF_MAXQ;
    ifEn.rcv.maxqlen    = IF_MAXQ;
    ifEn.open           = xifOpen;
    ifEn.close          = xifClose;
    ifEn.output         = pc_xif_output;
    ifEn.ioctl          = pc_xif_ioctl;
    ifEn.maxpackets     = MAX_PACKETS;

    if_register(&ifEn);

    c_conws("CE_ETH.XIF: CosmosEx network, " IF_NAME);
    char unit[2] = { '0' + ifEn.unit, 0 };
    c_conws(unit);
    c_conws(", MAC ");
    for(int i = 0; i < ETH_ALEN; i++) {
        printHexByte(info.mac[i]);
        c_conws(i < ETH_ALEN - 1 ? ":" : "\r\n");
    }

    return 0;
}
