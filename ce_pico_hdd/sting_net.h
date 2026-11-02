#ifndef __STING_NET_H__
#define __STING_NET_H__

#include <Arduino.h>

// Network for STinG (ce_sting/CE_ETH.STX) over raw ethernet frames on W6100 socket 0. The ACSI / SCSI commands of
// HOSTMOD_RAW_NET are answered here on the pico and not forwarded to the host. Protocol: ce_sting/README.md

#define HOSTMOD_RAW_NET             7

#define RAWNET_CMD_IDENTIFY         0x00
#define RAWNET_CMD_CONFIG           0x01
#define RAWNET_CMD_SEND             0x02
#define RAWNET_CMD_RECV             0x03
#define RAWNET_CMD_GET_STATS        0x04

bool stingNetCommand(uint8_t *cmd, uint32_t cmdLen);    // true if it was a HOSTMOD_RAW_NET command - then handled here, don't send to host
bool stingNetOwnsWrite(void);                           // true while the data of a DMA write belong to STinG SEND, not to host
void stingNetWriteData(uint8_t *data, uint32_t len);    // data of that DMA write
void stingNetProcess(void);                             // receive frames, answer ARP, resend ARP requests - call from core 0 loop

#endif
