#include <stdio.h>
#include "pico/stdlib.h"

#include "defs.h"
#include "display.h"
#include "utils.h"
#include "panel.h"

#define DISPLAY_FLUSH_MS    50      // displayMessage() waits at most this long for the panel UART

extern uint16_t version[2];

void displayInit(void)
{
    panelInit();

    char fw[24];
    sprintf(fw, "fw 20%02x-%02x-%02x", version[0] & 0xff, version[1] >> 8, version[1] & 0xff);

    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, "COSMO2EX FDD");
    panelSetLine(2, PANEL_CENTER, 0, fw);
    panelSetLine(3, PANEL_CENTER, 0, "starting");
    panelFlush(DISPLAY_FLUSH_MS);
}

// Up to 3 lines of text, the 4th line is cleared. Sent right away, as the callers often block the main loop after
// this (connecting to wifi / host).
void displayMessage(const char* msg1, const char* msg2, const char* msg3)
{
    panelSetLine(0, PANEL_LEFT, 0, msg1 ? msg1 : "");
    panelSetLine(1, PANEL_LEFT, 0, msg2 ? msg2 : "");
    panelSetLine(2, PANEL_LEFT, 0, msg3 ? msg3 : "");
    panelSetLine(3, PANEL_LEFT, 0, "");
    panelFlush(DISPLAY_FLUSH_MS);
}
