#include <Ethernet.h>
#include <EEPROM.h>

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "command_handling.h"
#include "connection.h"
#include "ipc.h"
#include "neopixel.h"

extern uint8_t cmd[17];  // received command bytes

volatile bool core1running = false;
volatile bool core0setupDone = false;   // set by core 0 at the end of setup() - SCSI starts listening on the bus then
extern volatile bool connected;

volatile uint8_t core1state = STATE_GET_COMMAND;

static void configPins(const int *pins, int count, bool output, const int *levels)
{
    for (int i = 0; i < count; i++)
    {
        if(output) {
            gpio_set_function(pins[i], GPIO_FUNC_SIO);
            gpio_set_dir(pins[i], GPIO_OUT);
            gpio_put(pins[i], levels[i]);
        } else {
            pinMode(pins[i], INPUT);
        }

        gpio_set_drive_strength(pins[i], GPIO_DRIVE_STRENGTH_12MA);
    }
}

void core1_setup(void)
{
    flash_safe_execute_core_init();     // call this for flash_safe_execute() to work

    debug("CORE 1 setup - %s\n", IS_ACSI ? "ACSI" : "SCSI");

    if(IS_ACSI) {
        // config pins as inputs
        const int inputs[]  = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7, PIN_CS, PIN_A1, PIN_ACK, PIN_RESET};
        configPins(inputs, sizeof(inputs) / sizeof(inputs[0]), false, NULL);

        // config pins as outputs
        const int outputs[] = {PIN_DATA_DIR, PIN_INT, PIN_DRQ};
        const int levels[]  = {           0,       1,       1};
        configPins(outputs, sizeof(outputs) / sizeof(outputs[0]), true, levels);
    } else {
        // config pins as inputs
        const int inputs[]  = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7, PIN_SEL_IO_DP_DS, PIN_RST_CD_REQ_CP, PIN_ACK};
        configPins(inputs, sizeof(inputs) / sizeof(inputs[0]), false, NULL);

        // config pins as outputs
        const int outputs[] = {PIN_DATA_DIR, PIN_IN_OE, PIN_OUT_OE, PIN_OUT_LE1, PIN_OUT_LE2};
        const int levels[]  = {           0,         1,          1,           0,           0};
        configPins(outputs, sizeof(outputs) / sizeof(outputs[0]), true, levels);
    }

    neopixelInit();     // before pioConfigAll(), so a failure there can be shown

    pioConfigAll();     // configure all PIO state machines

    resetBridge();
}

// ACSI: got the 1st command byte? SCSI: is the selection happening?
static bool gotFirstCmdByte(void)
{
    return IS_ACSI ? PIO_gotFirstCmdByte() : isSelectionHappening();
}

void core1_main_loop(void)
{
    core1_setup();

    uint32_t lastSendFwTime = millis();

    debug("CORE 1 main\n");
    core1running = true;

    bool listening = false;

    while(1)
    {
        if(!listening && core0setupDone) {      // init done - start answering the host (until then the SCSI neopixel is not disturbed by the bus)
            listening = true;
            busStartListening();
        }

        if(IS_ACSI && BIT_IS_L(PIN_RESET)) {   // when ACSI RESET is L, enter reset mode - no PIO transfers
            core1state = STATE_GET_COMMAND;
            pioConfig(MODE_RESET);
        }

        // get the command from ACSI / SCSI and send it to host
        // IN  STATE: STATE_GET_COMMAND
        // OUT STATE: WAIT_COMMAND_RESPONSE when GOOD, STATE_GET_COMMAND when FAIL
        if (core1state == STATE_GET_COMMAND)
        {
            if(gotFirstCmdByte())       // if 1st CMD byte was received / selection is happening
            {
                core1state = onGetCommand();
            }
            else
            {
                neopixelProcess();      // bus idle - the neopixel pin can be used now

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
        if(bfr) {
            // debug("c1: c %d\n", bfr->command);
            switch(bfr->command)
            {
                // read data to Atari
                case STATE_DATA_READ:
                {
                    timeoutStart(CMD_TIMEOUT_ONESECOND);    // next chunk from host must come in time - 100 ms was too short e.g. for a TCP retransmit (>= 200 ms)

                    bool res = onDataRead(bfr->length, bfr->data);

                    if(!res) {  // if failed, go to back to command receiving
                        core1state = STATE_GET_COMMAND;
                        timeoutClear();
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
            core1state = STATE_GET_COMMAND;
        }
    }
}
