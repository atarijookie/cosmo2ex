// CE_ETH.STX - STinG port driver for CosmosEx pico HDD (W6100 ethernet). See README.md.
//
// Structure taken from STinG's ports/ether (ether.c, tools.c). The pico does ARP and the ethernet frames, this driver
// only moves IP datagrams: STinG's send queue -> SEND command, RECV command -> STinG's receive queue.

#include <mint/osbind.h>
#include <mint/basepage.h>
#include <stdint.h>

#include "acsi.h"
#include "stdlib.h"

#include "sting.h"
#include "cenet.h"

#define M_YEAR      2026
#define M_MONTH     11
#define M_DAY       2
#define M_VERSION   "01.00"

#define RECV_ROUNDS         2       // max RECV commands in one receive() - more if the pico says more packets are waiting
#define POOL_RESERVE        8192    // leave this much of STinG's memory pool for the rest of STinG
#define DGRAM_OVERHEAD      64      // KRmalloc() of IP_DGRAM + its blocks, per packet

extern BASEPAGE *_base;

TPL *tpl;
STX *stx;

int16 my_set_state(PORT *port, int enable);
int16 my_cntrl(PORT *port, uint32 arg, int code);
void  my_send(PORT *port);
void  my_receive(PORT *port);

static PORT myPort = {
    "CosmosEx",         // name
    L_SER_BUS,          // type
    FALSE,              // active
    0,                  // flags
    0xffffffffUL,       // ip_addr
    0xffffffffUL,       // sub_mask
    1500,               // mtu
    1500,               // max_mtu
    0,                  // stat_sd_data
    NULL,               // send
    0,                  // stat_rcv_data
    NULL,               // receive
    0,                  // stat_dropped
    NULL,               // driver
    NULL                // next
};

static DRIVER myDriver = {
    pc_set_state,
    pc_cntrl,
    my_send,
    my_receive,
    "CosmosEx",
    M_VERSION,
    ((M_YEAR - 1980) << 9) | (M_MONTH << 5) | M_DAY,
    "Jookie",
    NULL,
    NULL
};

static const char *hwTypes[] = { "CosmosEx W6100", NULL };

static volatile int16 busy;     // user mode code is talking to the device, the timer calls must not
static uint8 deviceFound;
static uint8 infoValid;
static TNetInfo info;
static uint32 configuredIp;     // IP the pico knows
static uint8 rxNo = 1;          // number of the RECV batch to ask for, the same again after a failed transfer
static uint8 txNo = 1;

static uint8 rxMem[NET_BUF_SIZE + 16];      // in BSS - in ST RAM, as the program flags say no fast RAM
static uint8 txMem[NET_BUF_SIZE + 16];
static uint8 *rxBuf;
static uint8 *txBuf;

static const char fault[] = "CE_ETH.STX : STinG extension module. Only to be started by STinG !\r\n";

//--------------------------------------------------------------------------------

static uint16 getWord(const uint8 *p)
{
    return (p[0] << 8) | p[1];
}

static void putWord(uint8 *p, uint16 v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static void putDword(uint8 *p, uint32 v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void depleteQueue(IP_DGRAM **queue)
{
    IP_DGRAM *walk, *next;

    for(walk = *queue; walk; walk = next) {
        next = walk->next;
        IP_discard(walk, TRUE);
    }

    *queue = NULL;
}

//--------------------------------------------------------------------------------
// user mode -> supervisor helpers, the parameters go through these globals

static int16 superResult;

static long activateSuper(void)
{
    if(!deviceFound) {
        deviceFound = netFindDevice();
        if(!deviceFound) {
            return FALSE;
        }
    }

    if(netIdentify(&info) < 0) {
        return FALSE;
    }
    infoValid = TRUE;

    if(netConfig(RAWNET_CFG_ENABLE, myPort.ip_addr) != RAWNET_ST_OK) {
        return FALSE;
    }
    configuredIp = myPort.ip_addr;
    return TRUE;
}

static long deactivateSuper(void)
{
    if(deviceFound) {
        netConfig(0, 0);
    }
    return TRUE;
}

static long identifySuper(void)
{
    if(!deviceFound) {
        deviceFound = netFindDevice();
    }

    if(deviceFound && netIdentify(&info) >= 0) {
        infoValid = TRUE;
    }
    return infoValid;
}

//--------------------------------------------------------------------------------
// STinG driver functions

int16 my_set_state(PORT *port, int enable)
{
    if(port != &myPort) {
        return FALSE;
    }

    busy = TRUE;

    if(enable) {
        rxNo = txNo = 1;
        superResult = (int16) Supexec(activateSuper);
    } else {
        Supexec(deactivateSuper);
        depleteQueue(&myPort.send);
        depleteQueue(&myPort.receive);
        superResult = TRUE;
    }

    busy = FALSE;
    return superResult;
}

int16 my_cntrl(PORT *port, uint32 arg, int code)
{
    if(port != &myPort) {
        return E_PARAMETER;
    }

    switch(code) {
        case CTL_ETHER_GET_MAC:
            if(!infoValid) {
                busy = TRUE;
                Supexec(identifySuper);
                busy = FALSE;
            }
            if(!infoValid) {
                return E_UNREACHABLE;
            }
            memcpy((uint8 *) arg, info.mac, 6);
            return E_NORMAL;

        case CTL_ETHER_INQ_SUPPTYPE:
            *((const char ***) arg) = hwTypes;
            return E_NORMAL;

        case CTL_ETHER_SET_TYPE:
            return (arg == 0) ? E_NORMAL : E_PARAMETER;

        case CTL_ETHER_GET_TYPE:
            *((int16 *) arg) = 0;
            return E_NORMAL;
    }

    return E_FNAVAIL;           // CTL_ETHER_SET_MAC too - the pico decides the MAC
}

// may the timer call talk to the device now?
static uint8 canTalk(PORT *port)
{
    if(port != &myPort || !myPort.active || busy || !deviceFound) {
        return FALSE;
    }

    if(*FLOCK) {                // somebody uses the DMA (floppy, harddisk), don't wait in the timer, try on the next one
        return FALSE;
    }

    if(myPort.ip_addr != configuredIp) {            // IP changed in STinG meanwhile
        if(netConfig(RAWNET_CFG_ENABLE, myPort.ip_addr) != RAWNET_ST_OK) {
            return FALSE;
        }
        configuredIp = myPort.ip_addr;
    }

    return TRUE;
}

// one received IP packet into STinG's receive queue - like retrieve_dgram() in STinG's ports/ether/tools.c
static uint8 storeDgram(const uint8 *buf, uint16 length)
{
    if(length < sizeof(IP_HDR)) {
        return FALSE;
    }

    uint16 hdLen = (buf[0] & 0x0f) << 2;
    uint16 total = getWord(buf + 2);

    if(total > length || hdLen < sizeof(IP_HDR) || hdLen > total) {
        return FALSE;
    }

    IP_DGRAM *dgram = KRmalloc(sizeof(IP_DGRAM));
    if(!dgram) {
        return FALSE;
    }

    fastCopy(&dgram->hdr, buf, sizeof(IP_HDR));

    dgram->opt_length = hdLen - sizeof(IP_HDR);
    dgram->pkt_length = total - hdLen;
    dgram->options    = KRmalloc(dgram->opt_length);
    dgram->pkt_data   = KRmalloc(dgram->pkt_length);

    if(!dgram->options || !dgram->pkt_data) {
        IP_discard(dgram, TRUE);
        return FALSE;
    }

    fastCopy(dgram->options,  buf + sizeof(IP_HDR), dgram->opt_length);
    fastCopy(dgram->pkt_data, buf + hdLen, dgram->pkt_length);

    dgram->recvd = &myPort;
    dgram->next  = NULL;
    set_dgram_ttl(dgram);

    IP_DGRAM **prev = &myPort.receive;
    while(*prev) {
        prev = &(*prev)->next;
    }
    *prev = dgram;

    myPort.stat_rcv_data += length;
    return TRUE;
}

void my_receive(PORT *port)
{
    if(!canTalk(port)) {
        return;
    }

    uint32 bufMax = NET_BUF_SIZE;
    if(infoValid && bufMax > (uint32) info.maxBatch + RECV_HDR_SIZE) {
        bufMax = (uint32) info.maxBatch + RECV_HDR_SIZE;
    }

    for(int round = 0; round < RECV_ROUNDS; round++) {
        // ask only for what STinG can store - what stays on the pico is not lost, what the ST got and dropped is
        int32 room = KRgetfree(FALSE) - POOL_RESERVE;
        if(room < 2048) {
            return;
        }

        uint32 want = ((uint32) room < bufMax) ? (uint32) room : bufMax;
        uint8 sectors = want >> 9;

        int status = netRecv(rxBuf, sectors, rxNo);

        if(status < 0 || rxBuf[0] != 0xCE || rxBuf[1] != rxNo) {      // failed - ask for the same batch next time
            return;
        }

        rxNo++;                                     // got it, the next RECV tells the pico to drop it

        uint16 dataBytes = getWord(rxBuf + 2);
        uint16 pkts      = getWord(rxBuf + 4);
        uint16 waiting   = getWord(rxBuf + 6);

        uint32 pos = RECV_HDR_SIZE;
        uint32 end = RECV_HDR_SIZE + dataBytes;

        while(pkts-- > 0 && pos + 2 <= end) {
            uint16 len = getWord(rxBuf + pos);
            if(pos + 2 + len > end) {
                break;
            }

            if(!storeDgram(rxBuf + pos + 2, len)) {
                myPort.stat_dropped++;
            }

            pos += 2 + len + (len & 1);
        }

        if(waiting == 0) {
            return;
        }
    }
}

void my_send(PORT *port)
{
    if(!myPort.send || !canTalk(port)) {
        return;
    }

    uint32 network = myPort.ip_addr & myPort.sub_mask;
    uint32 pos = 0;
    uint8 pkts = 0;
    IP_DGRAM *dgram;

    while((dgram = myPort.send) != NULL && pkts < 255) {
        uint16 length = sizeof(IP_HDR) + dgram->opt_length + dgram->pkt_length;
        uint32 entry = 6 + length + (length & 1);           // [len16][next hop][datagram], padded to even

        if(pos + entry > NET_BUF_SIZE) {
            if(pkts == 0) {                                 // would never fit
                myPort.send = dgram->next;
                myPort.stat_dropped++;
                IP_discard(dgram, TRUE);
                continue;
            }
            break;                                          // the rest goes in the next send()
        }

        myPort.send = dgram->next;

        if(check_dgram_ttl(dgram) != E_NORMAL) {            // too old, the kernel took care of it
            continue;
        }

        // next hop - like xmit_dgram() in STinG's ports/ether/tools.c
        uint32 nextHop;
        if(dgram->hdr.ip_dest == 0xffffffffUL || (dgram->hdr.ip_dest & myPort.sub_mask) == network) {
            nextHop = dgram->hdr.ip_dest;
        } else if((dgram->ip_gateway & myPort.sub_mask) == network) {
            nextHop = dgram->ip_gateway;
        } else {
            myPort.stat_dropped++;
            IP_discard(dgram, TRUE);
            continue;
        }

        uint8 *p = txBuf + pos;
        putWord(p, length);
        putDword(p + 2, nextHop);
        fastCopy(p + 6, &dgram->hdr, sizeof(IP_HDR));
        fastCopy(p + 6 + sizeof(IP_HDR), dgram->options, dgram->opt_length);
        fastCopy(p + 6 + sizeof(IP_HDR) + dgram->opt_length, dgram->pkt_data, dgram->pkt_length);
        if(length & 1) {
            p[6 + length] = 0;
        }

        pos += entry;
        pkts++;
        myPort.stat_sd_data += length;
        IP_discard(dgram, TRUE);
    }

    if(pkts == 0) {
        return;
    }

    if(netSend(txBuf, pos, txNo, pkts) < 0) {           // lost - TCP sends it again
        myPort.stat_dropped += pkts;
    }
    txNo++;
}

//--------------------------------------------------------------------------------

static long getStingCookie(void)
{
    return (long) readCookie(STIK_COOKIE_MAGIC);
}

static void install(void)
{
    PORT *ports;
    DRIVER *driver;

    query_chains(&ports, &driver, NULL);

    myPort.driver = &myDriver;
    myDriver.basepage = _base;

    while(ports->next) {
        ports = ports->next;
    }
    ports->next = &myPort;

    while(driver->next) {
        driver = driver->next;
    }
    driver->next = &myDriver;
}

int main(void)
{
    if(strcmp(_base->p_cmdlin, "\012STinG_Load") != 0) {
        (void) Cconws(fault);
        return 1;
    }

    DRV_LIST *drivers = (DRV_LIST *) Supexec(getStingCookie);

    if(!drivers || strcmp(drivers->magic, STIK_DRVR_MAGIC) != 0) {
        return 1;
    }

    tpl = (TPL *) pc_call_l(drivers->get_dftab, (int32) TRANSPORT_DRIVER);
    stx = (STX *) pc_call_l(drivers->get_dftab, (int32) MODULE_DRIVER);

    if(!tpl || !stx) {
        return 1;
    }

    rxBuf = (uint8 *) (((uint32) rxMem + 15) & ~15UL);    // aligned for every DMA (TT SCSI needs longwords)
    txBuf = (uint8 *) (((uint32) txMem + 15) & ~15UL);

    install();

    Ptermres(0x100 + _base->p_tlen + _base->p_dlen + _base->p_blen, 0);
    return 0;
}
