#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "panel.h"
#include "buttons.h"
#include "captive_portal.h"

// Key events from the front panel. Left / right (and their long press) are free for browsing the images later.
void onPanelKey(char button, char event)
{
    debug("panel key: %c %c\n", button, event);

    if(button == 'E' && event == 'H') {     // long press of enter - start the captive portal, or leave it
        toggleCaptivePortal();
    }
}

void buttonsInit(void)
{
    gpio_init(PIN_CAPTIVE_BTN);
    gpio_set_dir(PIN_CAPTIVE_BTN, GPIO_IN);
    gpio_pull_up(PIN_CAPTIVE_BTN);          // there's also a pull-up on the board
}

// The button on the FDD board (to GND): a long press starts the captive portal, or leaves it - right when the time is
// reached, like the long press on the front panel. A short press does nothing for now.
void handleAllButtons(void)
{
    static bool rawState = false, stableState = false, longDone = false;
    static uint32_t rawChangeTime = 0, pressTime = 0;

    uint32_t now = millis();
    bool pressed = !gpio_get(PIN_CAPTIVE_BTN);

    if(pressed != rawState) {
        rawState = pressed;
        rawChangeTime = now;
    }

    if(rawState != stableState && (now - rawChangeTime) >= BTN_DEBOUNCE_MS) {
        stableState = rawState;
        pressTime = now;
        longDone = false;
    }

    if(stableState && !longDone && (now - pressTime) >= BTN_LONG_PRESS_MS) {
        longDone = true;
        toggleCaptivePortal();
    }
}
