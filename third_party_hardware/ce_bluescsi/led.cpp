#include "pico/stdlib.h"

#include "defs.h"
#include "board.h"
#include "led.h"
#include "utils.h"

#define LED_STATE_INIT      0
#define LED_STATE_ERROR     1
#define LED_STATE_RUNNING   2

static volatile uint8_t ledState = LED_STATE_INIT;
static volatile uint8_t ledInitStep = 0;
static volatile uint8_t ledErrorCode = 0;
static volatile bool ledHalted = false;     // haltWithError() was called - the error stays shown

#define INIT_ON_MS          100
#define INIT_PERIOD_MS      500

// error blinking: code x (on + off), then pause
#define ERR_BLINK_ON_MS     250
#define ERR_BLINK_OFF_MS    250
#define ERR_PAUSE_MS        1500

static bool ledAvailable = false;

void ledSetInitStep(uint8_t step)
{
    if(ledHalted) {
        return;
    }

    ledInitStep = step;
    ledErrorCode = 0;
    ledState = LED_STATE_INIT;
}

void ledSetError(uint8_t code)
{
    if(ledHalted) {
        return;
    }

    ledErrorCode = code;
    ledState = LED_STATE_ERROR;
}

void ledSetRunning(void)
{
    if(ledHalted) {
        return;
    }

    ledErrorCode = 0;
    ledState = LED_STATE_RUNNING;
}

void haltWithError(uint8_t code)
{
    ledSetError(code);
    ledHalted = true;

    if(get_core_num() == 1) {       // core 0 keeps showing it
        while(1) {
            tight_loop_contents();
        }
    }

    while(1) {                      // core 0 - keep the LED and the debug output going
        ledProcess();
#ifdef LOG_PIO
        debugFromQueue();
#endif
        sleep_ms(10);
    }
}

static bool currentLevel(uint32_t now)
{
    switch(ledState) {
        case LED_STATE_RUNNING:
            return true;

        case LED_STATE_ERROR: {
            uint32_t code = ledErrorCode;
            uint32_t blinksTime = code * (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS);
            uint32_t t = now % (blinksTime + ERR_PAUSE_MS);

            return (t < blinksTime) && ((t % (ERR_BLINK_ON_MS + ERR_BLINK_OFF_MS)) < ERR_BLINK_ON_MS);
        }

        default:                    // init - fast blinking
            return (now % INIT_PERIOD_MS) < INIT_ON_MS;
    }
}

void ledInit(void)
{
    if(BOARD_IS_WIFI) {             // the LED on the WiFi chip
        pinMode(LED_BUILTIN, OUTPUT);
        digitalWrite(LED_BUILTIN, LOW);
        ledAvailable = true;
    } else if(board.pinLed != 0xff) {
        gpio_init(board.pinLed);
        gpio_put(board.pinLed, 0);
        gpio_set_dir(board.pinLed, GPIO_OUT);
        ledAvailable = true;
    }
}

// only on change - on the Pico W each LED write is a transfer to the WiFi chip
void ledProcess(void)
{
    static int lastLevel = -1;

    if(!ledAvailable || get_core_num() != 0) {
        return;
    }

    int level = currentLevel(millis()) ? 1 : 0;
    if(level == lastLevel) {
        return;
    }
    lastLevel = level;

    if(BOARD_IS_WIFI) {
        digitalWrite(LED_BUILTIN, level ? HIGH : LOW);
    } else {
        gpio_put(board.pinLed, level);
    }
}
