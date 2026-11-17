#ifndef __BRIGDE_H__
#define __BRIGDE_H__

#include "defs.h"

// bridge functions return value
#define E_TimeOut 0
#define E_OK 1

#define DIR_RECV 0
#define DIR_SEND 1

#define MODE_UNKNOWN            0
#define MODE_RESET              1       // same as SELECTION
#define MODE_SCSI_SELECTION     1       // same as RESET - bus released, watching SEL / RST
#define MODE_CMD                2       // COMMAND phase
#define MODE_MSG_OUT            3
#define MODE_DMA_READ           4       // DATA IN
#define MODE_DMA_WRITE          5       // DATA OUT
#define MODE_STATUS             6
#define MODE_MSG_IN             7

// The SCSI target on the BlueSCSI v2 board - see bridge_scsi.cpp. The same API as the SCSI part of ce_pico_hdd/bridge.h.

void pioConfigAll(void);                // pins + PIO state machines - on core 1, at its start
int pioConfig(int newMode, bool force=false);      // set the phase / transfer mode, returns the previous mode

void resetBridge(void);                 // release the bus (BSY too), back to watching SEL
void busStartListening(void);           // start answering the selections - at the end of core 1 setup

uint8_t PIO_write(void);                // get one byte from the initiator (COMMAND, MESSAGE OUT)

void statusAndMsgRead(uint8_t scsiStatusByte);     // STATUS byte, then MESSAGE IN (COMMAND COMPLETE), then bus free

void DMA_write_startWithCount(uint32_t transfersCount);     // set how many bytes we will get, before DMA_write()
uint8_t DMA_write(void);                // get byte from the initiator
void DMA_writeBlock(uint8_t* bfr, uint32_t cnt);        // get cnt bytes, stops on timeout (brStat == E_TimeOut)

void DMA_read(uint8_t val);             // send byte to the initiator
void DMA_readBlock(const uint8_t* bfr, uint32_t cnt);   // send cnt bytes, stops on timeout (brStat == E_TimeOut)
void DMA_read_waitForEnd(void);

bool isAtnAsserted(void);               // valid only while we hold BSY (after getSelectionByte() returned an ID)
bool isBusResetAsserted(void);
const char* busStateString(void);      // for the logs on a timeout
uint8_t isSelectionHappening(void);     // SEL asserted, BSY not - some target is being selected
uint8_t getSelectionByte(void);         // our ID which got selected (BSY is asserted then), or 0xff

#endif
