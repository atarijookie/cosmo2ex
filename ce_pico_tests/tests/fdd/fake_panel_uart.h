// Fake of the front panel UART (ce_pico_fdd/panel_uart.cpp) and of the panel key handler (buttons.cpp).

#ifndef FAKE_PANEL_UART_H
#define FAKE_PANEL_UART_H

#include <string>
#include <vector>
#include <deque>
#include <cstddef>

extern std::string fakePanelTx;             // everything the FDD sent to the panel
extern size_t fakePanelWritable;            // what panelUartWritable() returns - SerialPIO has an 8 byte fifo
extern std::deque<char> fakePanelRx;        // sent by the panel, not read yet
extern std::vector<std::string> fakePanelKeys;     // onPanelKey() calls, as "<button><event>"

void fakePanelReset(void);
void fakePanelReceive(const std::string& data);

#endif
