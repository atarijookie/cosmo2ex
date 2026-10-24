#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "panel.h"
#include "buttons.h"
#include "captive_portal.h"
#include "neopixel.h"
#include "image_browser.h"

// Key events from the front panel. Left / right browse the images, enter inserts the shown one (image_browser.cpp).
// The long presses of left / right are not used.
void onPanelKey(char button, char event)
{
    debug("panel key: %c %c\n", button, event);

    if(browserKey(button, event)) {
        return;
    }

    if(button == 'E' && event == 'H') {     // long press of enter - start the captive portal, or leave it
        toggleCaptivePortal();
    }
}

void buttonsInit(void)
{
    gpio_init(PIN_CAPTIVE_BTN);
    gpio_set_dir(PIN_CAPTIVE_BTN, GPIO_IN);
    gpio_disable_pulls(PIN_CAPTIVE_BTN);    // 4k7 pull-down on the board - strong enough for the RP2350 erratum E9
}

// The button on the FDD board (to 3.3V, the pin is shared with the neopixel): a long press starts the captive portal,
// or leaves it - right when the time is reached, like the long press on the front panel. A short press does nothing for
// now. The neopixel saw the press as a long H on its data input, so its color is sent again after the release.
void handleAllButtons(void)
{
    static bool rawState = false, stableState = false, longDone = false;
    static uint32_t rawChangeTime = 0, pressTime = 0;

    uint32_t now = millis();
    bool pressed = gpio_get(PIN_CAPTIVE_BTN);

    if(pressed != rawState) {
        rawState = pressed;
        rawChangeTime = now;
    }

    if(rawState != stableState && (now - rawChangeTime) >= BTN_DEBOUNCE_MS) {
        stableState = rawState;
        pressTime = now;
        longDone = false;

        if(!stableState) {
            neopixelResend();
        }
    }

    if(stableState && !longDone && (now - pressTime) >= BTN_LONG_PRESS_MS) {
        longDone = true;
        toggleCaptivePortal();
    }
}
