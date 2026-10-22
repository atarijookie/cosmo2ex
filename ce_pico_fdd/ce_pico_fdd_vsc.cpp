#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
// #include "tusb.h"

#include "connection.h"
#include "display.h"
#include "ikbd.h"
#include "buttons.h"
#include "core1_main.h"
#include "mfm_write.h"
#include "psram.h"
#include "defs.h"
#include "utils.h"
#include "serial_config.h"
#include "ipc.h"
#include "written_sectors.h"
#include "host_receive.h"

extern volatile bool core1running;

uint16_t version[2] = {0xf025, 0x0901}; // this means: Franz, 2025-09-01
uint8_t atnSendFwVersion[ATN_SENDFWVERSION_LEN_TX];
uint8_t atnSendTrackRequest[ATN_SENDTRACK_REQ_LEN_TX];
uint8_t atnSendWholeImageRequest[TX_HEADER_SIZE];

void setupAtnBuffers(void);
void requestTrack(uint8_t side, uint8_t track);
void requestWholeImage(void);

TDrivePosition posStreamed, hwPosition;

uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

extern bool connectedToHost;

uint8_t mac[6];

int imageState = IMAGE_NOT_LOADED;
uint8_t imgTracks = 80, imgSides = 2, imgSectorsPerTrack = 9;
char imageFileName[32];
bool diskChanged = false;
bool diskChangeActive = false;  // pins inverted for the disk change now
uint32_t diskChangeStart;

void pio_uart_setup(void);

extern Settings_t Settings;

/*
    Notes on stdio output via USB + UART

    The stdio is currently configured to use USB and UART at the same time,
    for easy configuration via USB cable, plus debug strings via UART.
    But stdio functions (e.g. printf) may halt undefinitelly, if you
    don't have USB connected and the USB output buffer gets full.
    This is solved by:
    - using printf (goes to UART and USB) only on serial config mode
    - using debug (gotes to UART only) when sending just debug strings
    This way the serial config will work via USB, debug strings will
    go through UART and should never block indefinitelly.
*/

void waitForCore1Running(void)
{
    int loops = 0;

    while(true) {
        sleep_ms(100);
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

void setup(void)
{
    stdio_init_all();
    debug("\n\nsetup() starting\n");

    // it seems that get_absolute_time() returns 0 until the timer is used by sleep_ms() or similar,
    // doing short sleep_ms so that any additional millis() will work correctly.
    sleep_ms(1);

    #define INPUTS_COUNT 6
    int inputs[INPUTS_COUNT] = {PIN_DRIVE_SEL, PIN_MOT_EN, PIN_DIR, PIN_STEP, PIN_WGATE, PIN_SIDE1};

    for (int i = 0; i < INPUTS_COUNT; i++)
    {
        gpio_set_function(inputs[i], GPIO_FUNC_SIO);
        gpio_set_dir(inputs[i], GPIO_IN);
    }

    #define OUTPUTS_COUNT 7
    int outputs[OUTPUTS_COUNT] = {PIN_DENSITY, PIN_INDEX, PIN_TRACK00, PIN_WPROTECT, PIN_DSKCHG, PIN_FLCC_OE, PIN_CS};
    int levels[OUTPUTS_COUNT]  = {          0,         1,           0,            1,          0,           1,      1};

    for (int i = 0; i < OUTPUTS_COUNT; i++)
    {
        gpio_set_function(outputs[i], GPIO_FUNC_SIO);
        gpio_set_dir(outputs[i], GPIO_OUT);
        gpio_put(outputs[i], levels[i]);
    }

    displayInit();

    loadSettingsFromEeprom();

    // // If the flag to enter config mode is set we must store settings from PSRAM to EEPROM.
    // // In order for flashing to work, we must ensure that only 1 core is writing to flash and running, so
    // // we must enter config mode and storing to flash before we call cyw43_arch_init(),
    // // which runs on other core.
    // if(psramConfigFlagGet()) {
    //     debug("Storing settings to EEPROM\n");
    //     displayMessage("STORING SETTINGS");

    //     psramConfigFlagClear();
    //     storeSettingsFromPSRAMtoEEPROM();

    //     while(1);   // should not get here, because storeSettingsFromPSRAMtoEEPROM should restart
    // }

    setupAtnBuffers();

    // Initialise UART 1
    gpio_set_function(PIN_KEYB_TX, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX));
    gpio_set_function(PIN_KEYB_TX_ORIG, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX_ORIG));
    uart_init(uart1, 7812);

    // initialize additional UART RX via PIO and IRQ
    // pio_uart_setup();

    ipcInit();

    // start core 1
    multicore_reset_core1();
    multicore_fifo_drain();
    sleep_ms(10);
    multicore_launch_core1(core1_main_loop);    // start core1 for mfm stream handling
    waitForCore1Running();

    // Initialise the Wi-Fi chip
    if (cyw43_arch_init()) {
        debug("Wi-Fi init failed\n");
        while(1);
    }

    // Enable wifi station
    cyw43_arch_enable_sta_mode();
}

void requestTrack(uint8_t side, uint8_t track)
{
    atnSendTrackRequest[TX_HEADER_SIZE + 0] = side;
    atnSendTrackRequest[TX_HEADER_SIZE + 1] = track;
    sendHeaderAndDataToHost(atnSendTrackRequest, ATN_SENDTRACK_REQ_LEN_TX - TX_HEADER_SIZE);
}

void requestWholeImage(void)
{
    debug("requestWholeImage");
    sendHeaderAndDataToHost(atnSendWholeImageRequest, 0);
}

uint32_t lastSendFwTime;

void sendFwReport(uint32_t now)
{
    lastSendFwTime = now;
    sendHeaderAndDataToHost(atnSendFwVersion, ATN_SENDFWVERSION_LEN_TX - TX_HEADER_SIZE);
}

int main()
{
    setup();

    uint32_t now = millis();
    lastSendFwTime = now;
    uint32_t lastInputCheck = now;

    while(1)
    {
        now = millis();

        if((now - lastInputCheck) > 250) {
            lastInputCheck = now;

            // if(tud_mounted()) {         // USB connected and ready?
            //     int key = getchar_timeout_us(0);
            //     if(key == '\n' || key == '\r' || strlen(Settings.ssid) == 0) {
            //         serialConfigLoop();
            //     }
            // }
        }

        // connect to wifi, discover CE server, connect to CE server
        connectToHost();

        // handle any data incoming
        handleIncommingData();

        // send heartbeat (fw version) once a second
        if (connectedToHost && (now - lastSendFwTime) >= 1000)
        {
            sendFwReport(now);
        }

        // send sectors written by ST to host - also the ones kept while not connected
        writtenSectorsHandle();

        // request whole image if no image loaded - but only after all written sectors were sent,
        // so the image we get has them (e.g. after reconnect)
        if(connectedToHost && imageState == IMAGE_NOT_LOADED && !writtenSectorsPending())
        {
            // before requesting the whole image, send fw report, so the host will get mac address,
            // which he will use to identify this device
            sendFwReport(now);

            // now do the image request
            imageState = IMAGE_REQUESTED;
            requestWholeImage();
        }

        // when disk change happened
        if(diskChanged) {
            diskChanged = false;        // no change anymore

            if(!diskChangeActive) {     // pins not inverted yet? invert them - if they are, inverting again would undo it
                diskChangeActive = true;

                BIT_INVERT(PIN_DENSITY);    // invert pins
                BIT_INVERT(PIN_WPROTECT);
                BIT_INVERT(PIN_DSKCHG);
                // debug("DSK CHG start");
            }

            diskChangeStart = now;      // invert back 1 s after the last disk change
        }

        // after enough time passed since the disk change, need to invert pins back
        if(diskChangeActive && (now - diskChangeStart) >= 1000) {
            diskChangeActive = false;   // don't do this until next disk change

            BIT_INVERT(PIN_DENSITY);    // invert pins
            BIT_INVERT(PIN_WPROTECT);
            BIT_INVERT(PIN_DSKCHG);
            // debug("DSK CHG end");
        }

        //-------------------------------------------------
        now = millis();

        //---------------------------
        // check the button state and press duration
        handleAllButtons();

        // handle ikbd data transfer
        // ikbdHandling();
    }
}

void setupAtnBuffers(void)
{
    // firmware report / keep alive / heartbeat
    memset(atnSendFwVersion, 0, ATN_SENDFWVERSION_LEN_TX);
    storeHeader(atnSendFwVersion, ATN_FW_VERSION, 0);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE, version[0]);
    storeWord(atnSendFwVersion + TX_HEADER_SIZE + 2, version[1]);
    atnSendFwVersion[TX_HEADER_SIZE + 4] = DEV_FEATURE_FDD + DEV_FEATURE_IKBD;    // features bits - ACSI + IKBD

    // one track request
    memset(atnSendTrackRequest, 0, ATN_SENDTRACK_REQ_LEN_TX);
    storeHeader(atnSendTrackRequest, ATN_SEND_TRACK, 2);

    // all tracks request
    storeHeader(atnSendWholeImageRequest, ATN_SEND_WHOLE_IMAGE, 0);
}

void storeMacAddress(void)
{
    memset(mac, 0, 6);
    cyw43_hal_get_mac(CYW43_HAL_MAC_WLAN0, mac);            // get mac into the mac array
    memcpy(atnSendFwVersion + TX_HEADER_SIZE + 6, mac, 6);  // copy mac into fw version array
    debug("mac: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
