#ifndef STATUS_LED_H_
#define STATUS_LED_H_

#include <cstdint>

// The WS2812 neopixel on the RP2040-Zero (GP16): orange during setup, green once the first screen was drawn.

#define LED_ORANGE      0x40, 0x10, 0x00
#define LED_GREEN       0x00, 0x30, 0x00

void ledInit(void);
void ledSet(uint8_t r, uint8_t g, uint8_t b);   // takes ~0.33 ms - call only on a change

#endif
