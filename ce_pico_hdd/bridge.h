#ifndef __BRIGDE_H__
#define __BRIGDE_H__

#include "defs.h"

// bridge functions return value
#define E_TimeOut 0
#define E_OK 1
#define E_OK_A1 2
#define E_CARDCHANGE 3
#define E_RESET 4
#define E_FAIL_CMD_AGAIN 5

#define DIR_RECV 0
#define DIR_SEND 1

#define MODE_UNKNOWN            0
#define MODE_RESET              1       // same as SELECTION
#define MODE_SCSI_SELECTION     1       // same as RESET
#define MODE_CMD                2       // SCSI: COMMAND phase, ACSI: the command bytes after the 1st one
#define MODE_CMD_REST           MODE_CMD
#define MODE_MSG_OUT            3       // SCSI only
#define MODE_DMA_READ           4
#define MODE_DMA_WRITE          5
#define MODE_STATUS             6
#define MODE_MSG_IN             7       // SCSI only
#define MODE_ACSI_FIRST         8       // ACSI only - waiting for the 1st command byte

#define BIT_ATN     1
#define BIT_RESET   2

// which variant of the board this is, see detectHwVariant()
#define VARIANT_SCSI    0
#define VARIANT_ACSI    1

extern uint8_t hwVariant;
#define IS_ACSI         (hwVariant == VARIANT_ACSI)

void detectHwVariant(void);         // read the variant strap - call before any other bus pin gets configured

//--------------------------------------------------------
// common for both variants, they call the ACSI or SCSI implementation below

void pioConfigAll(void);
int pioConfig(int newMode, bool force=false);

void resetBridge(void);

uint8_t PIO_write(void);            // get next CMD byte from ST

void statusAndMsgRead(uint8_t scsiStatusByte); // send status byte to ST, on SCSI followed by MSG IN

void DMA_write_startWithCount(uint32_t transfersCount);     // set how many bytes we will transfer, before calling DMA_write()
uint8_t DMA_write(void);            // get byte from ST using DMA
void DMA_writeBlock(uint8_t* bfr, uint32_t cnt);        // get cnt bytes from ST using DMA, stops on timeout (brStat == E_TimeOut)

void DMA_read(uint8_t val);         // send byte to ST using DMA
void DMA_readBlock(const uint8_t* bfr, uint32_t cnt);   // send cnt bytes to ST using DMA, stops on timeout (brStat == E_TimeOut)
void DMA_read_waitForEnd(void);

void neopixelOutput(uint32_t grb);  // send the color to the neopixel - call only while the bus is idle
void busStartListening(void);       // start answering the Atari once connected to host - SCSI ignores the bus until then, ACSI listens from the start

//--------------------------------------------------------
// ACSI only

uint8_t PIO_gotFirstCmdByte(void);  // check if we got the 1st command byte
uint8_t PIO_writeFirst(void);       // get 1st CMD byte from ST  -- without setting INT

//--------------------------------------------------------
// SCSI only

bool isAtnAsserted(void);
uint8_t isSelectionHappening(void); // check if the selection is happening
uint8_t getSelectionByte(void);     // get the selected ID of this device

//--------------------------------------------------------
// implementations of the common functions - bridge_acsi.cpp, bridge_scsi.cpp

void acsiPioConfigAll(void);
int acsiPioConfig(int newMode, bool force);
void acsiResetBridge(void);
uint8_t acsiPIO_write(void);
void acsiStatusRead(uint8_t scsiStatusByte);
void acsiDMA_write_startWithCount(uint32_t transfersCount);
uint8_t acsiDMA_write(void);
void acsiDMA_writeBlock(uint8_t* bfr, uint32_t cnt);
void acsiDMA_read(uint8_t val);
void acsiDMA_readBlock(const uint8_t* bfr, uint32_t cnt);
void acsiDMA_read_waitForEnd(void);
void acsiNeopixelOutput(uint32_t grb);

void scsiPioConfigAll(void);
int scsiPioConfig(int newMode, bool force);
void scsiResetBridge(void);
uint8_t scsiPIO_write(void);
void scsiStatusAndMsgRead(uint8_t scsiStatusByte);
void scsiDMA_write_startWithCount(uint32_t transfersCount);
uint8_t scsiDMA_write(void);
void scsiDMA_writeBlock(uint8_t* bfr, uint32_t cnt);
void scsiDMA_read(uint8_t val);
void scsiDMA_readBlock(const uint8_t* bfr, uint32_t cnt);
void scsiDMA_read_waitForEnd(void);
void scsiNeopixelOutput(uint32_t grb);
void scsiStartListening(void);

#endif
