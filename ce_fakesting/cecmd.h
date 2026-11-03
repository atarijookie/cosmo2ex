#ifndef _CECMD_H_
#define _CECMD_H_

// Talking to the CosmosEx network module (HOSTMOD_NETWORK_ADAPTER) through libacsiscsi.
//
// The STinG API is called by the applications in user mode, but hdIf.cmd() and getTicks() of libacsiscsi need
// supervisor mode (DMA chip registers, the 200 Hz counter at 0x4ba). The VBL routine is already in supervisor mode.

#include <stdint.h>

// a command to the device - like hdIf.cmd(), from user mode or from the VBL
void     ceCmd(uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount);

// 200 Hz ticks, from user mode or from the VBL
uint32_t ceTicks(void);

// DMA can't reach this buffer directly: it's in TT fast RAM, or not aligned as the interface needs
// (even on ACSI and Falcon, longword on TT SCSI) - go through FastRAMBuffer instead
uint8_t  ceNeedsBounce(const void *buffer);

// find the network module: the device CE_DD uses (CEPI cookie) first, then scan the buses.
// On success sets deviceID, the interface (hdd_if_select()), requiredVersion and localIP. Returns TRUE / FALSE.
uint8_t  ceFindNetDevice(void);

#endif
