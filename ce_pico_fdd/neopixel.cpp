#include "defs.h"
#include "utils.h"
#include "neopixel.h"

#define LED_STATE_INIT      0
#define LED_STATE_ERROR     1
#define LED_STATE_RUNNING   2
#define LED_STATE_PORTAL    3

static volatile uint8_t ledState = LED_STATE_INIT;
static volatile uint8_t ledInitStep = 0;
static volatile uint8_t ledErrorCode = 0;
static volatile bool ledImageLoaded = false;
static volatile bool resendWanted = false;

// colors at full brightness of the status - the WS2812C-2020 is bright, so these are far from 255 (as on ce_pico_hdd)
#define ORANGE_R            96
#define ORANGE_G            28
#define GREEN_G             48
#define RED_R               64
#define NO_IMAGE_RGB        6       // dim white - connected, no image yet
#define PORTAL_B            64

// error blinking: code x (on + off), then pause
#define ERR_BLINK_ON_MS     250
#define ERR_BLINK_OFF_MS    250
#define ERR_PAUSE_MS        1500

void ledSetInitStep(uint8_t step)
{
    ledInitStep = step;
    ledErrorCode = 0;
    ledState = LED_STATE_INIT;
}

void ledSetError(uint8_t code)
{
    ledErrorCode = code;
    ledState = LED_STATE_ERROR;
}

void ledSetRunning(bool imageLoaded)
{
    ledImageLoaded = imageLoaded;
    ledErrorCode = 0;
    ledState = LED_STATE_RUNNING;
}

void ledSetCaptivePortal(void)
{
    ledErrorCode = 0;
    ledState = LED_STATE_PORTAL;
}

void ledSetFromConnection(bool ssidStored, bool wifiFailed, bool wifiConnected, bool hostFound, bool hostConnected,
                          bool imageLoaded)
{
    if(hostConnected) {
        ledSetRunning(imageLoaded);
    } else if(!ssidStored) {
        ledSetError(LED_ERR_NO_SSID);
    } else if(!wifiConnected) {
        if(wifiFailed) {
            ledSetError(LED_ERR_NO_WIFI);
        } else {
            ledSetInitStep(LED_STEP_WIFI);
        }
    } else if(!hostFound) {
        ledSetInitStep(LED_STEP_DISCOVERY);
    } else {
        ledSetInitStep(LED_STEP_CONNECTING);
    }
}

static uint32_t toGrb(uint8_t r, uint8_t g, uint8_t b)
{
    return (((uint32_t) g) << 16) | (((uint32_t) r) << 8) | b;
}

static uint32_t currentColor(uint32_t now)
{
    switch(ledState) {
        case LED_STATE_RUNNING:
            return ledImageLoaded ? toGrb(0, GREEN_G, 0) : toGrb(NO_IMAGE_RGB, NO_IMAGE_RGB, NO_IMAGE_RGB);

        case LED_STATE_PORTAL:
            return toGrb(0, 0, PORTAL_B);

        case LED_STATE_ERROR: {
            uint32_t code = ledErrorCode;
            uint32_t blinksTime = code * (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS);
            uint32_t t = now % (blinksTime + ERR_PAUSE_MS);

            bool on = (t < blinksTime) && ((t % (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS)) < ERR_BLINK_ON_MS);
            return on ? toGrb(RED_R, 0, 0) : 0;
        }

        default: {                  // init - orange, brighter with each step
            uint32_t step = MIN(MAX(ledInitStep, 1), LED_STEP_COUNT);
            return toGrb((ORANGE_R * step) / LED_STEP_COUNT, (ORANGE_G * step) / LED_STEP_COUNT, 0);
        }
    }
}

static uint32_t lastColor;
static uint32_t lastSentTime;

void neopixelInit(void)
{
    lastColor = 0xffffffff;         // not a color - the first neopixelProcess() sends
    lastSentTime = millis() - NEOPIXEL_GAP_MS;
    resendWanted = false;

    neopixelOutputInit();
}

void neopixelResend(void)
{
    resendWanted = true;
}

void neopixelProcess(void)
{
    uint32_t now = millis();
    uint32_t color = currentColor(now);

    if(color == lastColor && !resendWanted) {   // nothing changed
        return;
    }

    if((now - lastSentTime) < NEOPIXEL_GAP_MS) {    // the previous frame may not be latched yet - on the next pass
        return;
    }

    resendWanted = false;
    neopixelOutput(color);
    lastColor = color;
    lastSentTime = now;
}
