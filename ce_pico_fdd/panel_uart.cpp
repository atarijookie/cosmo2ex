// UART to the front panel - PIO, as uart0 is the debug port and uart1 the keyboard

#include <Arduino.h>
#include <SerialPIO.h>

#include "defs.h"
#include "panel.h"

static SerialPIO panelSerial(PIN_PANEL_TX, PIN_PANEL_RX, 128);     // 128 = rx buffer size

void panelUartInit(void)
{
    panelSerial.begin(PANEL_UART_BAUD);
    gpio_pull_up(PIN_PANEL_RX);         // no panel connected - stays idle instead of receiving noise
}

size_t panelUartWritable(void)
{
    int n = panelSerial.availableForWrite();
    return (n > 0) ? n : 0;
}

size_t panelUartWrite(const char* data, size_t len)
{
    return panelSerial.write((const uint8_t*) data, len);
}

int panelUartRead(void)
{
    return panelSerial.read();
}
