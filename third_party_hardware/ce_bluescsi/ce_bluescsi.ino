/*
    ce_bluescsi - CosmosEx HDD (SCSI) on the BlueSCSI v2 board, with the network instead of the SD card.
    See readme.md. The host protocol is the same as ce_pico_hdd, the host sees it as a SCSI HDD.

    Arduino IDE settings needed to compile it (the same as FQBN_BLUESCSI in firmware/compile.sh and in readme.md - keep
    them in sync; the release file is ce_pico_hdd_BLUESCSI-PICO-2W.uf2):

    - core: Raspberry Pi Pico/RP2040/RP2350 by Earle F. Philhower (arduino-pico), version 5.4.2
            (Boards Manager URL: https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json)
    - no extra libraries - all used ones come with the core
    - WiFi credentials: copy secrets.example.h to secrets.h and fill it in (secrets.h is not in git)

    Tools menu:
    - Board:                Raspberry Pi Pico 2W
    - CPU Architecture:     ARM
    - Flash Size:           4MB (Sketch: 2MB, FS: 2MB)  - the FS is needed for the firmware update from the web UI (new +
                                                          previous firmware); another FS size = different flash layout,
                                                          the web update refuses it and the device must be flashed via USB
    - CPU Speed:            150 MHz
    - Optimize:             Small (-Os) (standard)
    - Operating System:     None
    - Profiling:            Disabled
    - RTTI:                 Disabled
    - Stack Protector:      Disabled
    - C++ Exceptions:       Disabled
    - Debug Port:           Disabled
    - Debug Level:          None
    - USB Stack:            Pico SDK  - NOT "No USB": with it the WiFi connection to the host stalls (see ce_pico_fdd/readme.md)
    - IP/Bluetooth Stack:   IPv4 Only
    - WiFi Region:          Worldwide
    - Upload Method:        Picoprobe/Debugprobe (CMSIS-DAP)  - or hold BOOTSEL and copy the .uf2 onto the RP2350 drive

    After changing a .pio file run firmware/pioasm.sh third_party_hardware/ce_bluescsi - the Arduino IDE doesn't
    regenerate the *.pio.h.
*/

#include "pico/stdlib.h"

#include "defs.h"
#include "board.h"
#include "bridge.h"
#include "utils.h"
#include "connection.h"
#include "ipc.h"
#include "led.h"
#include "net.h"
#include "fw_update.h"
#include "boot_status.h"
#include "core1_main_loop.h"

// Core 1 (setup1() + loop1()) handles the SCSI bus, core 0 (setup() + loop()) the rest.
// The WiFi chip and lwIP are started by the arduino core before setup(), they run in the background (IRQ) on core 0.

bool core1_separate_stack = true;   // core 1 gets its own 8 kB stack instead of the 4 kB scratch one

uint16_t version[2] = {0xa026, 0x1117}; // this means: hAns, 2026-11-17

uint8_t atnSendFwVersion[ATN_SENDFWVERSION_LEN_TX];
uint8_t atnSendACSIcommand[ATN_SENDACSICOMMAND_LEN_TX];

extern volatile bool core1running;
extern volatile bool connected;
volatile bool core0ready = false;   // board detected, settings + IPC ready - core 1 may start using them

static void setupAtnBuffers(void);

static void waitForCore1Running(void)
{
    int loops = 0;

    while(!core1running) {
#ifdef LOG_PIO
        debugFromQueue();
#endif
        ledProcess();
        sleep_ms(10);

        if(++loops >= 100) {
            loops = 0;
            debug("CORE 0 waiting for CORE 1\n");
        }
    }
}

void setup(void)
{
    boardDetect();      // first - the pins depend on it
    debugInit();
    ledInit();
    ledSetInitStep(LED_STEP_SETUP);

    debug("\n\n------------\nCORE 0 setup - BlueSCSI with %s, %s\n", board.name, rp2040.isPicoW() ? "WiFi chip found" : "no WiFi chip");

    loadSettings();
    debug("enabledIDs: %02X\n", settings.enabledIDs);

    fwInfoInit();       // identity, size + MD5 of this firmware for the update in the web UI; counts the boots of a new firmware

    setupAtnBuffers(); // fill the ATN buffers with needed headers and terminators
    ipcInit();

    // core 1 was started by the arduino core before setup(), it waits for this
    core0ready = true;
    waitForCore1Running();
    ledSetInitStep(LED_STEP_CORE1);

    if(!netStart()) {   // no network - the drive can't work, the boot status tells it to the Atari
        bool noConfig = netState() == NETST_NO_CONFIG;
        bootStatusSet(noConfig ? BOOTST_NET_NO_CONFIG : BOOTST_NET_NO_HW);
        haltWithError(noConfig ? LED_ERR_NO_WIFI_CONFIG : LED_ERR_NO_NET_HW);
    }

    netGetMac(atnSendFwVersion + TX_HEADER_SIZE + 6);   // the host knows the device by this MAC
    uint8_t* mac = atnSendFwVersion + TX_HEADER_SIZE + 6;
    debug("mac: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    connectionInit();
    ledSetInitStep(LED_STEP_NETWORK);
    bootStatusSet(BOOTST_NET_JOINING);
}

// core 1 - the SCSI bus
void setup1(void)
{
    while(!core0ready) {        // board, settings and IPC are set up by core 0
        tight_loop_contents();
    }

    core1Setup();
}

void loop1(void)
{
    core1Loop();
}

static void setupAtnBuffers(void)
{
    storeHeader(atnSendACSIcommand, ATN_ACSI_COMMAND, 0);

    memset(atnSendFwVersion, 0, ATN_SENDFWVERSION_LEN_TX);
    storeHeader(atnSendFwVersion, ATN_FW_VERSION, 0);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE, version[0]);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE + 2, version[1]);
    atnSendFwVersion[TX_HEADER_SIZE + 4] = DEV_FEATURE_SCSI;    // features bits - SCSI, no IKBD
    atnSendFwVersion[TX_HEADER_SIZE + 5] = 0x42;                // v.4, SCSI
}

void loop(void)
{
    uint8_t header[TX_HEADER_SIZE];

#ifdef LOG_PIO
    debugFromQueue();
#endif

    // network, discover CE server, connect to CE server
    connectToHost();

    // handle any data incoming
    handleIncommingData();

    if(connected) {     // LED on when connected
        ledSetRunning();
    }
    ledProcess();

    // something in the queue for core0? get it, handle it
    IPCbuffer* bfr = ipcGetBufferFromFifo(0);
    if(bfr) {
        switch(bfr->command)
        {
            // report FW version to host
            case STATE_SEND_FW_VER:
                sendHeaderAndDataToHost(atnSendFwVersion, ATN_SENDFWVERSION_LEN_TX - TX_HEADER_SIZE);
                fwInfoSendIfNeeded();   // once per connection - after the FW version, which tells the host our MAC
                break;

            // send SCSI command to host
            case STATE_GET_COMMAND:
                memcpy(atnSendACSIcommand + TX_HEADER_SIZE, bfr->data, bfr->length);
                sendHeaderAndDataToHost(atnSendACSIcommand, ATN_SENDACSICOMMAND_LEN_TX - TX_HEADER_SIZE);
                break;

            // create and send one header at the start
            case STATE_SEND_WRITE_MORE_DATA:
                storeHeader(header, ATN_WRITE_MORE_DATA, bfr->length);
                sendDataToHost(header, TX_HEADER_SIZE);
                break;

            // should write this data to host
            case STATE_DATA_WRITE:
                sendDataToHost(bfr->data, bfr->length);
                break;
        }

        ipcReleaseBuffer(bfr);
    }

    fwTrialLoop(connected); // new firmware after an update: confirm it once connected to host, or reboot (rollback)
}
