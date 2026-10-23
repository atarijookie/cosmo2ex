#ifndef __PANEL_H__
#define __PANEL_H__

#include <cstdint>
#include <cstddef>

// Front panel (ce_pico_display) over UART - see ce_pico_display/protocol.md.
// Keeps what should be on the panel, sends only the changed lines, resends everything when the panel (re)announces
// itself. Sending never waits (except panelFlush()), the messages are queued and sent as the UART has space.

#define PANEL_LINES         4

#define PANEL_LEFT          'L'
#define PANEL_CENTER        'C'
#define PANEL_RIGHT         'R'

#define PANEL_BOLD          0x01
#define PANEL_INVERTED      0x02
#define PANEL_DOUBLE        0x04
#define PANEL_SCROLL        0x08        // added automatically when the text is longer than the panel line

#define PANEL_TEXT_MAX      63

void panelInit(void);
void panelClear(void);
void panelSetLine(int line, char justify, uint8_t flags, const char* text);
void panelLoop(void);                   // receive, send what's queued - call from the main loop
void panelFlush(uint32_t timeoutMs);    // send the changes now, wait until sent (at most timeoutMs)

bool panelPresent(void);                // got the I message from the panel
int  panelLines(void);

// key event from the panel: button 'L' / 'E' / 'R', event 'S' (short) / 'H' (long) - in buttons.cpp
void onPanelKey(char button, char event);

// UART to the panel - panel_uart.cpp (faked in the tests)
void   panelUartInit(void);
size_t panelUartWritable(void);                     // bytes that can be written now without waiting
size_t panelUartWrite(const char* data, size_t len);
int    panelUartRead(void);                         // -1 if nothing received

#endif
