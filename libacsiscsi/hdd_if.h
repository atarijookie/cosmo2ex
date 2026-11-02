#ifndef _HDD_IF_H_
#define _HDD_IF_H_

#include "global.h"

void scsiCmdTT           (uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount);
void scsi_cmd_Falcon       (uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount);

typedef void  (*THddIfCmd) (uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount);
typedef void  (*TsetReg)   (int whichReg, uint32_t value);
typedef uint32_t (*TgetReg)   (int whichReg);

typedef uint8_t  (*TdmaDataTx_prepare) (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);
typedef uint8_t  (*TdmaDataTx_do)      (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);

uint32_t scsiGetRegTT(int whichReg);
void  scsiSetRegTT(int whichReg, uint32_t value);

uint32_t scsiGetRegFalcon(int whichReg);
void  scsiSetRegFalcon(int whichReg, uint32_t value);

void  scsi_clrBit(int whichReg, uint32_t bitMask);
void  scsi_setBit(int whichReg, uint32_t bitMask);

uint8_t dmaDataTx_prepare_TT       (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);
uint8_t dmaDataTx_do_TT            (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);

uint8_t dmaDataTx_prepare_Falcon   (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);
uint8_t dmaDataTx_do_Falcon        (uint8_t readNotWrite, uint8_t *buffer, uint32_t dataByteCount);

typedef struct {
    THddIfCmd   cmd;
    THddIfCmd   cmd_nolock;
    THddIfCmd   cmdIntern;

    uint8_t     success;
    uint8_t     statusByte;
    uint8_t     phaseChanged;

    int         retriesDoneCount;
    int         maxRetriesCount;

    uint8_t     forceFlock;

    TsetReg     pSetReg;
    TgetReg     pGetReg;

    TdmaDataTx_prepare  pDmaDataTx_prepare;
    TdmaDataTx_do       pDmaDataTx_do;

    uint8_t     scsiHostId;
    uint8_t     showLogs;

    uint32_t    longTimeoutTicks;       // how long the timeout will be when timeout is set using scsiSetLongTimeout(). Default value SCSI_TIMEOUT_LONG.

    // Which low address bits must be 0 in a DMA buffer: 1 = even address (ACSI, Falcon SCSI), 3 = longword (TT SCSI - its
    // DMA ignores the low 2 bits, so a buffer at ...2 got the data 2 bytes lower, over whatever was before it).
    // scsiCmdTT() bounces small unaligned transfers through its own buffer; for big ones the caller must use an aligned
    // buffer (or a middle buffer). Last in the struct - other programs get to hdIf through the CEPI cookie.
    uint8_t     dmaAlignMask;

    // ACSI / SCSI ID of the CosmosEx found by CE_DD, for other programs which find hdIf through the CEPI cookie (CE_ETH.STX).
    // Older CE_DD doesn't have this field - the readers must check the ID they got (and the value is 0xff when not found).
    uint8_t     ceDeviceId;
} THDif;

extern THDif hdIf;

//--------------------------------
#define IF_NONE         0
#define IF_ACSI         1
#define IF_SCSI_TT      2
#define IF_SCSI_FALCON  3

void hdd_if_select(int ifType);     // call this function with above define as a param to init the pointers depending on that interface
//--------------------------------

void hdIfCmdAsUser(uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount);

#endif
