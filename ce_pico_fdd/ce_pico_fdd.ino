#include <WiFi.h>

#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/uart.h"

#include "connection.h"
#include "display.h"
#include "ikbd.h"
#include "buttons.h"
#include "core1_main.h"
#include "mfm_write.h"
#include "psram.h"
#include "defs.h"
#include "utils.h"
#include "settings.h"
#include "serial_config.h"
#include "ipc.h"
#include "written_sectors.h"
#include "host_receive.h"
#include "panel.h"

// Core 1 (setup1() + loop1()) does the floppy streaming and write capture, core 0 (setup() + loop()) the rest.
// The WiFi chip and lwIP are started by the arduino core before setup(), they run in the background (IRQ) on core 0.

bool core1_separate_stack = true;   // core 1 gets its own 8 kB stack instead of the 4 kB scratch one - IRQs nest deep there

extern volatile bool core1running;
volatile bool core0ready = false;   // pins set up, IPC ready - core 1 may start using them

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

/*
    Notes on serial output via USB + UART

    The USB serial (Serial) is used only for the serial config mode, debug strings go to UART0 (debug()).
    Writing to the USB serial may wait when USB is connected but nobody reads it, so only the config mode uses it -
    debug strings through the UART never block for long.
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
    // uart0 for debug strings (debug() writes to it directly)
    uart_init(uart0, 115200);
    gpio_set_function(PIN_TXD_DEBUG, UART_FUNCSEL_NUM(uart0, PIN_TXD_DEBUG));
    gpio_set_function(PIN_RXD_DEBUG, UART_FUNCSEL_NUM(uart0, PIN_RXD_DEBUG));

    debug("\n\nsetup() starting\n");

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
    buttonsInit();

    loadSettingsFromEeprom();

    setupAtnBuffers();

    // Initialise UART 1
    gpio_set_function(PIN_KEYB_TX, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX));
    gpio_set_function(PIN_KEYB_TX_ORIG, UART_FUNCSEL_NUM(uart1, PIN_KEYB_TX_ORIG));
    uart_init(uart1, 7812);

    // initialize additional UART RX via PIO and IRQ
    // pio_uart_setup();

    ipcInit();

    // core 1 was started by the arduino core before setup(), it waits for this
    core0ready = true;
    waitForCore1Running();

    WiFi.mode(WIFI_STA);
}

// core 1 - floppy streaming, write capture
void setup1(void)
{
    while(!core0ready) {        // pins and IPC buffers are set up by core 0
        tight_loop_contents();
    }

    core1Setup();
}

void loop1(void)
{
    core1Loop();
}

void requestTrack(uint8_t side, uint8_t track)
{
    atnSendTrackRequest[TX_HEADER_SIZE + 0] = side;
    atnSendTrackRequest[TX_HEADER_SIZE + 1] = track;
    sendHeaderAndDataToHost(atnSendTrackRequest, ATN_SENDTRACK_REQ_LEN_TX - TX_HEADER_SIZE);
}

void requestWholeImage(void)
{
    debug("requestWholeImage\n");
    sendHeaderAndDataToHost(atnSendWholeImageRequest, 0);
}

uint32_t lastSendFwTime;

void sendFwReport(uint32_t now)
{
    lastSendFwTime = now;
    sendHeaderAndDataToHost(atnSendFwVersion, ATN_SENDFWVERSION_LEN_TX - TX_HEADER_SIZE);
}

void loop(void)
{
    uint32_t now = millis();
    lastSendFwTime = now;
    uint32_t lastInputCheck = now;

    while(1)
    {
        now = millis();

        if((now - lastInputCheck) > 250) {
            lastInputCheck = now;

            // if(Serial) {                // USB serial connected?
            //     int key = Serial.read();
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

        // front panel: send the display changes, receive the key events
        panelLoop();

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
    WiFi.macAddress(mac);                                   // get mac into the mac array
    memcpy(atnSendFwVersion + TX_HEADER_SIZE + 6, mac, 6);  // copy mac into fw version array
    debug("mac: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
