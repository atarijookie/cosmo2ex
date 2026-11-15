/*
    ce_pico_hdd - CosmosEx HDD (ACSI / SCSI, detected on boot) on the W6100-EVB-Pico

    Arduino IDE settings needed to compile it (the same as FQBN_HDD in firmware/compile.sh and in readme.md - keep
    them in sync; the release file is ce_pico_hdd_W6100-EVB-PICO.uf2):

    - core: Raspberry Pi Pico/RP2040/RP2350 by Earle F. Philhower (arduino-pico), version 5.4.2
            (Boards Manager URL: https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json)
    - library: the patched Ethernet library from third_party/Ethernet - symlink it into the Arduino libraries dir, e.g.
            mklink /D "%USERPROFILE%\Documents\Arduino\libraries\Ethernet" "<repo>\third_party\Ethernet"
            (must win over the stock Ethernet library)

    Tools menu:
    - Board:                Raspberry Pi Pico
    - Flash Size:           2MB (Sketch: 1536KB, FS: 512KB)  - the FS is needed for the firmware update from the web UI;
                                                                another FS size = different flash layout, the web update
                                                                refuses it and the device must be flashed via USB
    - CPU Speed:            133 MHz
    - Optimize:             Small (-Os) (standard)
    - Operating System:     None
    - Profiling:            Disabled
    - RTTI:                 Disabled
    - Stack Protector:      Disabled
    - C++ Exceptions:       Disabled
    - Debug Port:           Disabled
    - Debug Level:          None
    - USB Stack:            No USB
    - IP/Bluetooth Stack:   IPv4 Only
    - Upload Method:        Picoprobe/Debugprobe (CMSIS-DAP)  - or hold BOOTSEL and copy the .uf2 onto the RPI-RP2 drive

    After changing a .pio file run firmware/pioasm.sh ce_pico_hdd - the Arduino IDE doesn't regenerate the *.pio.h.
*/

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
#include "sting_net.h"
#include "pin_test.h"
#include "fw_update.h"

uint16_t version[2] = {0xa026, 0x1114}; // this means: hAns, 2026-11-14

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
    debugDongleDetect();    // first - before anything that can hang; ~5 ms; J1 pin 2-3 loopback = debug output to KEYB_TX, IKBD off
    detectHwVariant();  // before anything touches the bus pins - their function depends on the variant
    ledSetInitStep(LED_STEP_SETUP);

    debugInit();
    debug("\n\n------------\nCORE 0 setup - %s variant\n", IS_ACSI ? "ACSI" : "SCSI");
    debug("debug output: %s\n", debugToIkbd ? "KEYB_TX (debug dongle / DEBUG_TO_IKBD)" : "LED pin");

    // TEMPORARY - endless test of the bus pins (shorts / open pins), uncomment only for testing the board
    // if(IS_ACSI) pinTestAcsi();

    loadSettings();
    debug("enabledIDs: %02X\n", settings.enabledIDs);

    fwInfoInit();       // identity, size + MD5 of this firmware for the update in the web UI; counts the boots of a new firmware
                        // (before core 1 starts - it may write to flash)

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
                    fwInfoSendIfNeeded();   // once per connection - after the FW version, which tells the host our MAC
                    break;

                // send ACSI command to host
                case STATE_GET_COMMAND:
                    if(stingNetCommand(bfr->data, bfr->length)) {      // network command of CE_ETH.STX, answered here, not by host
                        break;
                    }
                    memcpy(atnSendACSIcommand + TX_HEADER_SIZE, bfr->data, bfr->length);
                    sendHeaderAndDataToHost(SOCK_HDD, atnSendACSIcommand, ATN_SENDACSICOMMAND_LEN_TX - TX_HEADER_SIZE);
                    break;

                // create and send one header at the start
                case STATE_SEND_WRITE_MORE_DATA:
                    if(stingNetOwnsWrite()) {           // DMA write of STinG SEND, host doesn't know about it
                        break;
                    }
                    storeHeader(header, ATN_WRITE_MORE_DATA, bfr->length);
                    sendDataToHost(SOCK_HDD, header, TX_HEADER_SIZE);
                    break;

                // should write this data to host
                case STATE_DATA_WRITE:
                    if(stingNetOwnsWrite()) {
                        stingNetWriteData(bfr->data, bfr->length);
                        break;
                    }
                    sendDataToHost(SOCK_HDD, bfr->data, bfr->length);
                    break;
            }

            ipcReleaseBuffer(bfr);
        }

        ikbdProcessing();

        stingNetProcess();      // raw ethernet frames for STinG

        fwTrialLoop(connected); // new firmware after an update: confirm it once connected to host, or reboot (rollback)
    }
}
