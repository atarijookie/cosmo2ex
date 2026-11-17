// SCSI commands from the initiator to the host - the SCSI part of ce_pico_hdd/command_handling.cpp. Core 1.

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "command_handling.h"
#include "scsi.h"
#include "ipc.h"
#include "boot_status.h"

static uint8_t onGetCommandScsi(void);
static void getCmdLengthFromCmdBytesScsi(uint8_t cmd);

extern volatile bool connected;

//----------
uint8_t cmd[17];   // received command bytes - up to 16 B CDB + 1 B ICD marker
uint8_t cmdLen; // length of received command
uint8_t brStat; // status from bridge

uint8_t onGetCommand(void)
{
    // retrieve the command
    uint8_t good = onGetCommandScsi();

    if (!good)  // if failed to get the cmd, quit
    {
        timeoutClear();
        return STATE_GET_COMMAND;
    }

    if (!connected && bootStatusCommand(cmd, cmdLen))   // no host yet - answered here (boot status sector, not ready)
    {
        timeoutClear();
        return STATE_GET_COMMAND;
    }

    IPCbuffer* bfr = ipcGetFreeBuffer(0, CMD_TIMEOUT_SHORT);
    if(bfr == NULL) {
        debug("onGetCommand - no free buffers\n");
        resetBridge();          // we hold BSY - let the initiator go
        timeoutClear();
        return STATE_GET_COMMAND;
    }

    // store data to buffer, add buffer index to queue
    ipcSetBufferAndPutToFifo(bfr, 0, STATE_GET_COMMAND, cmdLen, cmd, cmdLen);

    //----------------
    // command received, send it to host
    timeoutStart(CMD_TIMEOUT_HOST); // the host must answer within this time

    return STATE_WAIT_COMMAND_RESPONSE;
}

// MESSAGE OUT phase for each message the initiator wants to send (e.g. IDENTIFY) - the messages are ignored.
// The previous mode is NOT restored like in ce_pico_hdd: before the command it's the selection mode, which releases BSY
// here - the caller sets the next phase anyway.
static void doMsgOutIfAtnSet(void)
{
    while(isAtnAsserted() && brStat == E_OK)
    {
        pioConfig(MODE_MSG_OUT);
        uint8_t msg = PIO_write();      // do 1 PIO write, ignore retrieved MSG byte
        (void) msg;
    }
}

static uint8_t onGetCommandScsi(void)
{
    uint8_t id = getSelectionByte(); // check if one of the IDs of this device is selected - BSY is asserted then

    if (id == 0xff)     // this device was not selected?
    {
        return 0;
    }

    doMsgOutIfAtnSet();     // do MSG_OUT if ATN set before cmd transfer

    if (brStat != E_OK)
    {
        debug("FAIL msg out\n");
        resetBridge();
        return 0;
    }

    pioConfig(MODE_CMD);

    cmdLen = 6; // maximum 6 bytes at start, but this might change in getCmdLengthFromCmdBytes()

    for (int i = 0; i < cmdLen; i++)
    {                         // receive the next command bytes
        cmd[i] = PIO_write();

        if (brStat != E_OK)
        { // if something was wrong, quit, failed
            debug("FAIL cmd i: %d\n", i);
            resetBridge();
            return 0;
        }

        if (i == 0)
        {                                         // if we got the 1st byte
            getCmdLengthFromCmdBytesScsi(cmd[0]); // we set up the length of command, etc.
        }
    }

    // now fix the command if the length is more than 6 bytes
    if (cmdLen > 6)
    {
        for (int i = cmdLen; i > 0; i--)
        { // move the cmd one byte further (to make cmd[0] unused)
            cmd[i] = cmd[i - 1];
        }
        cmd[0] = 0x1f; // store ICD command marker

        cmdLen++; // now the command is one byte longer
    }

    // host receives just 14 command bytes (see ATN_SENDACSICOMMAND_LEN_TX), so 16 B CDBs (17 B in ICD form) get truncated -
    // the whole CDB was still read from the bus, and the host answers to these unsupported commands with CHECK CONDITION
    cmdLen = MIN(cmdLen, ATN_SENDACSICOMMAND_LEN_TX - TX_HEADER_SIZE);

    // for all commands add fake ACSI ID on top of the 0th byte
    cmd[0] = cmd[0] | (id << 5); // add ID on the top 3 bits

    doMsgOutIfAtnSet();     // do MSG_OUT if ATN set after cmd transfer

    if (brStat != E_OK)
    {
        debug("FAIL msg out after cmd\n");
        resetBridge();
        return 0;
    }

    return 1;
}

bool onDataRead(uint32_t cnt, uint8_t* bfr)
{
    pioConfig(MODE_DMA_READ);

    DMA_readBlock(bfr, cnt);        // send all the data from buffer to Atari

    if (brStat == E_TimeOut)
    {
        debug("onDataRead TO 1 - %u B chunk, %s\n", cnt, busStateString());
        resetBridge();
        return false;
    }

    DMA_read_waitForEnd();

    if (brStat == E_TimeOut)        // read failed to wait for end?
    {
        debug("onDataRead TO 2 - %s\n", busStateString());
        resetBridge();
        return false;
    }

    return true;
}

// A buffer to core 0. It sends the written data over WiFi, which sometimes stalls for a while (TCP retransmit) - wait for
// a free buffer as long as the command timeout allows (the Atari waits too, REQ stays H meanwhile).
static IPCbuffer* getBufferForCore0(void)
{
    while(!hasTimedOut && !isBusResetAsserted()) {
        IPCbuffer* bfr = ipcGetFreeBuffer(0, 10);
        if(bfr) {
            return bfr;
        }
    }

    return NULL;
}

bool onDataWrite(uint32_t dataCnt)
{
    // create and send one header at the start
    IPCbuffer* bfr = getBufferForCore0();
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 0, STATE_SEND_WRITE_MORE_DATA, dataCnt, NULL, 0);
    } else {
        debug("onDataWrite - no free buffer till timeout (header)\n");
        resetBridge();
        return false;
    }

    if(dataCnt == 0) {              // nothing to transfer (host still waits for the header above), don't start the DMA - PIO would take 0 - 1 as the count
        return true;
    }

    pioConfig(MODE_DMA_WRITE);

    DMA_write_startWithCount(dataCnt);      // let PIO program know the count of bytes we want to transfer

    while (dataCnt > 0)             // something to write?
    {
        uint32_t cntNow = MIN(dataCnt, 512);

        dataCnt -= cntNow;

        // get buffer where we can store the written data
        IPCbuffer* bfr = getBufferForCore0();
        if(!bfr) {
            debug("onDataWrite - no free buffer till timeout\n");
            resetBridge();
            return false;
        }

        DMA_writeBlock(bfr->data, cntNow);      // get data from Atari

        if (brStat == E_TimeOut)
        {                              // if timeout occured
#ifdef LOG_MORE
            debug("onDataWrite timeout on DMA_write\n");
#endif
            ipcReleaseBuffer(bfr);          // buffer not sent to core 0, return it to the pool
            resetBridge();
            return false; // transfer failed, don't send status, just get next command
        }

        ipcSetBufferAndPutToFifo(bfr, 0, STATE_DATA_WRITE, cntNow, NULL, 0);
    }

    return true;  // continue with sending the status
}

void onReadStatus(uint8_t statusByte)
{
    statusAndMsgRead(statusByte);       // send the status to Atari, then bus free
}

static void getCmdLengthFromCmdBytesScsi(uint8_t cmd)
{
    switch ((cmd & 0xe0) >> 5) // get the length of the command
    {
    case 0:
        cmdLen = 6;
        break;
    case 1:
        cmdLen = 10;
        break;
    case 2:
        cmdLen = 10;
        break;
    case 4:
        cmdLen = 16;
        break;
    case 5:
        cmdLen = 12;
        break;
    default:
        cmdLen = 6;
        break;
    }
}

uint8_t idIsEnabled(uint8_t id)
{
    if (id > 7)
    {
        return false;
    }

    return (settings.enabledIDs & (1 << id));
}
