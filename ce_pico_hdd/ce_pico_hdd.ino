#include <Ethernet.h>
#include <EEPROM.h>

#include "pico/multicore.h"
#include "pico/sync.h"

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "command_handling.h"
#include "connection.h"
#include "ipc.h"
#include "ikbd.h"
#include "neopixel.h"

uint16_t version[2] = {0xa026, 0x1022}; // this means: hAns, 2026-10-22

uint8_t atnSendFwVersion[ATN_SENDFWVERSION_LEN_TX];
uint8_t atnSendACSIcommand[ATN_SENDACSICOMMAND_LEN_TX];

EthernetClient client;

extern volatile bool core1running;
extern volatile bool connected;
void core1_main_loop(void);

void waitForCore1Running(void)
{
    int loops = 0;

    while(true) {
        #ifdef LOG_LED
        debugFromQueue();
        #endif

        delay(100);
        loops++;

        if(loops >= 10) {
            loops = 0;
            debug("CORE 0 waiting for CORE 1\n");
        }

        if(core1running) {
            break;
        }
    }
}

void ethernetInit(void)
{
    debug("ethernetInit start\n");
    ledSetInitStep(LED_STEP_ETHERNET);

    Ethernet.init(17);              // WIZnet W6100-EVB-Pico

    while(true) {
        int gotIp = Ethernet.begin(settings.mac, 10000);    // set mac, get IP via DHCP, wait at most 10 s

        if(Ethernet.hardwareStatus() == EthernetNoHardware) {
            debug("No ethernet. HALT!\n");
            haltWithError(LED_ERR_NO_ETHERNET);
        }

        if(gotIp) {                 // got IP from DHCP, can continue
            break;
        }

        // without IP there's nothing to do, keep trying
        if(Ethernet.linkStatus() == LinkOFF) {
            debug("cable not connected\n");
            ledSetError(LED_ERR_NO_LINK);
        } else {
            debug("no IP from DHCP, retrying\n");
            ledSetError(LED_ERR_NO_DHCP);
        }

        sleep_ms(500);
    }

    debug("eth ip: %s\n", Ethernet.localIP().toString().c_str());
    ledSetInitStep(LED_STEP_GOT_IP);

    // store mac to fw version buffer
    memcpy(atnSendFwVersion + TX_HEADER_SIZE + 6, settings.mac, 6);

    debug("mac: %02X:%02X:%02X:%02X:%02X:%02X\n", atnSendFwVersion[TX_HEADER_SIZE + 6], atnSendFwVersion[TX_HEADER_SIZE + 7], atnSendFwVersion[TX_HEADER_SIZE + 8],
                                                  atnSendFwVersion[TX_HEADER_SIZE + 9], atnSendFwVersion[TX_HEADER_SIZE + 10], atnSendFwVersion[TX_HEADER_SIZE + 11]);
}

void setup(void)
{
    detectHwVariant();  // before anything touches the bus pins - their function depends on the variant
    ledSetInitStep(LED_STEP_SETUP);

    debugInit();
    debug("\n\n------------\nCORE 0 setup - %s variant\n", IS_ACSI ? "ACSI" : "SCSI");

    loadSettings();
    debug("enabledIDs: %02X\n", settings.enabledIDs);

    setupAtnBuffers(); // fill the ATN buffers with needed headers and terminators
    ipcInit();

    // start core 1
    multicore_reset_core1();
    multicore_fifo_drain();
    sleep_ms(10);
    multicore_launch_core1(core1_main_loop);
    waitForCore1Running();
    ledSetInitStep(LED_STEP_CORE1);

    ikbdInit();

    ethernetInit();     // ethernet init
}

void setupAtnBuffers(void)
{
    storeHeader(atnSendACSIcommand, ATN_ACSI_COMMAND, 0);

    memset(atnSendFwVersion, 0, ATN_SENDFWVERSION_LEN_TX);
    storeHeader(atnSendFwVersion, ATN_FW_VERSION, 0);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE, version[0]);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE + 2, version[1]);
    if(IS_ACSI) {
        atnSendFwVersion[TX_HEADER_SIZE + 4] = DEV_FEATURE_ACSI + DEV_FEATURE_IKBD;    // features bits - ACSI + IKBD
        atnSendFwVersion[TX_HEADER_SIZE + 5] = 0x41;                    // v.4, ACSI
    } else {
        atnSendFwVersion[TX_HEADER_SIZE + 4] = DEV_FEATURE_SCSI + DEV_FEATURE_IKBD;    // features bits - SCSI + IKBD
        atnSendFwVersion[TX_HEADER_SIZE + 5] = 0x42;                    // v.4, SCSI
    }
}

void loop(void)
{
    uint8_t header[TX_HEADER_SIZE];

    debug("CORE 0 main\n");

    while(1)
    {
#ifdef LOG_LED
        debugFromQueue();
#endif

        // discover CE server, connect to CE server
        connectToHost();

        // handle any data incoming
        handleIncommingData();

        if(connected) {     // green when connected, the brightest orange while looking for host / connecting
            ledSetRunning();
        } else {
            ledSetInitStep(LED_STEP_CONNECTING);
        }

        // something in the queue for core0? get it, handle it
        IPCbuffer* bfr = ipcGetBufferFromFifo(0);
        if(bfr) {
            // debug("c0: c %d\n", bfr->command);
            switch(bfr->command)
            {
                // report FW version to host
                case STATE_SEND_FW_VER:
                    sendHeaderAndDataToHost(SOCK_HDD, atnSendFwVersion, ATN_SENDFWVERSION_LEN_TX - TX_HEADER_SIZE);
                    break;

                // send ACSI command to host
                case STATE_GET_COMMAND:
                    memcpy(atnSendACSIcommand + TX_HEADER_SIZE, bfr->data, bfr->length);
                    sendHeaderAndDataToHost(SOCK_HDD, atnSendACSIcommand, ATN_SENDACSICOMMAND_LEN_TX - TX_HEADER_SIZE);
                    break;

                // create and send one header at the start
                case STATE_SEND_WRITE_MORE_DATA:
                    storeHeader(header, ATN_WRITE_MORE_DATA, bfr->length);
                    sendDataToHost(SOCK_HDD, header, TX_HEADER_SIZE);
                    break;

                // should write this data to host
                case STATE_DATA_WRITE:
                    sendDataToHost(SOCK_HDD, bfr->data, bfr->length);
                    break;
            }

            ipcReleaseBuffer(bfr);
        }

        ikbdProcessing();
    }
}
