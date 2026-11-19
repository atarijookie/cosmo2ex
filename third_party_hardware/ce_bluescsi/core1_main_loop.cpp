// Core 1 - the SCSI bus. Started by the arduino core via setup1() / loop1() in ce_bluescsi.ino.

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "command_handling.h"
#include "ipc.h"
#include "core1_main_loop.h"

extern uint8_t cmd[17];  // received command bytes

volatile bool core1running = false;
extern volatile bool connected;

volatile uint8_t core1state = STATE_GET_COMMAND;

static uint32_t lastSendFwTime;

// After the bridge gave up on a command (bus / host timeout), the host may still be sending its data and status. Those
// buffers are dropped - put on the bus they would end up as the data of the next command. New selections wait till
// the old answer is drained: its status came, or nothing came for DRAIN_QUIET_MS.
#define DRAIN_QUIET_MS      2000

static bool draining;
static uint32_t drainLastActivity;
static uint32_t drainDropped;

// the current READ, for the log when it fails
static uint32_t readBytesDone;
static uint32_t readChunkCount;
static uint32_t readLastChunkMs;
static uint32_t readMaxGapMs;
static uint32_t cmdStartMs;         // when the command went to the host
static bool gotAnswer;              // the first buffer of the host's answer came

static void startDraining(const char* why)
{
    debug("core1 - gave up on cmd (%s) %u ms after it, read: %u B in %u chunks, max gap %u ms - dropping the rest of the answer\n",
          why, millis() - cmdStartMs, readBytesDone, readChunkCount, readMaxGapMs);

    resetBridge();      // release the bus (BSY) - no isSelectionHappening() call, which would do it, while draining

    draining = true;
    drainLastActivity = millis();
    drainDropped = 0;
}

static void stopDraining(const char* why)
{
    debug("core1 - drain done (%s), dropped %u buffers\n", why, drainDropped);
    draining = false;
}

static void newCommandStats(void)
{
    readBytesDone = 0;
    readChunkCount = 0;
    readLastChunkMs = millis();
    readMaxGapMs = 0;
    cmdStartMs = millis();
    gotAnswer = false;
}

void core1Setup(void)
{
    debug("CORE 1 setup\n");

    pioConfigAll();     // pins + PIO state machines

    resetBridge();

    busStartListening(); // answers right away - while not connected with the boot status (boot_status.cpp)

    lastSendFwTime = millis();

    debug("CORE 1 main\n");
    core1running = true;
}

void core1Loop(void)
{
    // get the command from SCSI and send it to host
    // IN  STATE: STATE_GET_COMMAND
    // OUT STATE: WAIT_COMMAND_RESPONSE when GOOD, STATE_GET_COMMAND when FAIL
    if (draining && (millis() - drainLastActivity) >= DRAIN_QUIET_MS) {
        stopDraining("quiet");
    }

    if (core1state == STATE_GET_COMMAND)
    {
        if(!draining && isSelectionHappening())       // if the selection is happening
        {
            newCommandStats();
            core1state = onGetCommand();
        }
        else
        {
            uint32_t now = millis();

            if ((now - lastSendFwTime) >= 1000)
            {
                lastSendFwTime = now;

                // if connected, keep sending SEND_FW_VER commands
                if(connected)
                {
                    IPCbuffer* bfr = ipcGetFreeBuffer(0, CMD_TIMEOUT_SHORT);
                    if(bfr) {
                        ipcSetBufferAndPutToFifo(bfr, 0, STATE_SEND_FW_VER, 0, NULL, 0);
                    }
                }
            }
        }
    }

    // something in the queue for core1? get it, handle it
    IPCbuffer* bfr = ipcGetBufferFromFifo(1);
    if(bfr && core1state != STATE_WAIT_COMMAND_RESPONSE) {  // answer to a command we gave up on (or no command at all) - drop it
        if(!draining) {     // log once, then drain - the rest of this answer may still be coming
            debug("core1 - stale answer from host (cmd %d, %u B), dropping it\n", bfr->command, bfr->length);
            draining = true;
            drainDropped = 0;
        }

        drainDropped++;
        drainLastActivity = millis();

        if(draining && bfr->command == STATE_READ_STATUS) {   // the end of the old answer
            stopDraining("status");
        }

        ipcReleaseBuffer(bfr);
        bfr = NULL;
    }

    if(bfr && !gotAnswer) {     // how long the host took to answer - for the log, if it's slow
        gotAnswer = true;
        uint32_t took = millis() - cmdStartMs;

        if(took >= 300) {
            debug("core1 - host answered after %u ms\n", took);
        }
    }

    if(bfr) {
        switch(bfr->command)
        {
            // read data to Atari
            case STATE_DATA_READ:
            {
                uint32_t now = millis();
                readMaxGapMs = MAX(readMaxGapMs, now - readLastChunkMs);
                readLastChunkMs = now;

                timeoutStart(CMD_TIMEOUT_HOST);         // next chunk from host must come in time - a TCP retransmit takes >= 200 ms

                bool res = onDataRead(bfr->length, bfr->data);

                if(!res) {  // if failed, go to back to command receiving
                    core1state = STATE_GET_COMMAND;
                    timeoutClear();
                    startDraining("data in");
                } else {
                    readBytesDone += bfr->length;
                    readChunkCount++;
                }
                break;
            }

            // write data from Atari to device
            case STATE_DATA_WRITE:
            {
                uint32_t dataCnt = bfr->length;

                longTimeout_basedOnSectorCount(dataCnt >> 9); // set timeout time based on how many sectors are transfered
                bool res = onDataWrite(dataCnt);

                if(!res) {  // if failed, go to back to command receiving
                    core1state = STATE_GET_COMMAND;
                    timeoutClear();
                    startDraining("data out");
                }
                break;
            }

            // this happens after READ - send status byte to ST (read)
            case STATE_READ_STATUS:
            {
                timeoutStart();             // start the timeout timer to give the rest of code full timeout time
                uint8_t statusByte = bfr->data[0];
                onReadStatus(statusByte);
                core1state = STATE_GET_COMMAND;  // get the next command
                timeoutClear();             // clear timeout, no need for it
                break;
            }
        }

        ipcReleaseBuffer(bfr);
    }

    // if the data from host doesn't come within timeout, quit
    if (hasTimedOut)
    {
        timeoutClear();

#ifdef LOG_MORE
        debug("State : %d, cmd: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X, timeout at: %d\n",
            core1state, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7], cmd[8], cmd[9], cmd[10], cmd[11], millis());
#endif
        if(core1state == STATE_WAIT_COMMAND_RESPONSE) {    // the host didn't answer in time - its late answer must not go to the bus
            core1state = STATE_GET_COMMAND;
            startDraining("host timeout");
        }

        core1state = STATE_GET_COMMAND;     // the next isSelectionHappening() releases the bus (BSY)
    }
}
