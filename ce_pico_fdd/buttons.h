#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#define BTN_DEBOUNCE_MS     20
#define BTN_LONG_PRESS_MS   1000    // same as the long press on the front panel

void buttonsInit(void);
void handleAllButtons(void);        // the button on the FDD board - call from the main loop

void startCaptivePortal(void);

#endif
