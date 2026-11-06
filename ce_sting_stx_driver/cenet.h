#ifndef _CENET_H_
#define _CENET_H_

// Talking to the HOSTMOD_RAW_NET part of ce_pico_hdd (ce_pico_hdd/sting_net.cpp) over ACSI / SCSI. Protocol: README.md
// All functions here must run in supervisor mode.

#include "cetypes.h"

#define HOSTMOD_RAW_NET         7

#define RAWNET_CMD_IDENTIFY     0x00
#define RAWNET_CMD_CONFIG       0x01
#define RAWNET_CMD_SEND         0x02
#define RAWNET_CMD_RECV         0x03
#define RAWNET_CMD_GET_STATS    0x04

#define RAWNET_CFG_ENABLE       0x01
#define RAWNET_CFG_BROADCAST    0x02
#define RAWNET_CFG_FRAMES       0x04        // whole ethernet frames, from protocol version RAWNET_VERSION_FRAMES
#define RAWNET_VERSION_FRAMES   2

#define RAWNET_ST_OK            0x00
#define RAWNET_ST_EMPTY         0x01
#define RAWNET_ST_RX_PENDING    0x80

#define RECV_HDR_SIZE           16
#define NET_BUF_SIZE            8192        // each of RX and TX DMA buffer

typedef struct {
    uint16  version;
    uint8   mac[6];
    uint8   link;
    uint16  maxBatch;
} TNetInfo;

uint32 readCookie(uint32 id);                                   // value of cookie, 0 if not found
void  netSetDmaBuffer(uint8 *buf);                              // 512 B for IDENTIFY, needed when this program's BSS may not be in ST RAM
uint8 netFindDevice(void);                                      // scan the buses for the device, TRUE when found
int   netIdentify(TNetInfo *info);                              // status, -1 when the command failed
int   netConfig(uint8 flags, uint32 ip);
int   netSend(uint8 *buf, uint32 len, uint8 batchNo, uint8 pktCount);
int   netRecv(uint8 *buf, uint8 maxSectors, uint8 batchNo);

#endif
