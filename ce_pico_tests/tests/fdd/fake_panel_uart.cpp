// Fake of the front panel UART, see fake_panel_uart.h

#include "fake_panel_uart.h"
#include "panel.h"

std::string fakePanelTx;
size_t fakePanelWritable = 8;
std::deque<char> fakePanelRx;
std::vector<std::string> fakePanelKeys;

void fakePanelReset(void)
{
    fakePanelTx.clear();
    fakePanelWritable = 8;
    fakePanelRx.clear();
    fakePanelKeys.clear();
}

void fakePanelReceive(const std::string& data)
{
    fakePanelRx.insert(fakePanelRx.end(), data.begin(), data.end());
}

void panelUartInit(void)
{
}

size_t panelUartWritable(void)
{
    return fakePanelWritable;
}

size_t panelUartWrite(const char* data, size_t len)
{
    fakePanelTx.append(data, len);
    return len;
}

int panelUartRead(void)
{
    if (fakePanelRx.empty()) {
        return -1;
    }
    char c = fakePanelRx.front();
    fakePanelRx.pop_front();
    return (unsigned char) c;
}

void onPanelKey(char button, char event)
{
    fakePanelKeys.push_back(std::string(1, button) + event);
}
