#ifndef HUB75_H_
#define HUB75_H_

#include <cstdint>

// HUB75 LED panels instead of the OLED (pico_display_hub75 board): the PIO shows the frame from RAM over and over,
// fed by 2 chained DMA channels - no CPU time for the refresh. hub75Show() builds the next frame and swaps.

bool hub75Detect(void);                 // true on the HUB75 board - call before anything drives the pins
void hub75Init(void);
void hub75Show(const uint8_t* buf);     // the u8g2 buffer, see hub75_frame.h
void hub75SetBrightness(uint8_t value); // 0..255, shown right away

#endif
