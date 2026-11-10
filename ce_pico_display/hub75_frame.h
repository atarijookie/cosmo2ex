#ifndef HUB75_FRAME_H_
#define HUB75_FRAME_H_

// The u8g2 frame buffer (128x32, SSD1306 layout) -> the word stream the hub75 PIO program shows (see hub75.pio).
// Hardware independent, tested on the host.

#include <cstdint>

#define HUB75_WIDTH         128     // 2 panels of 64x32, chained
#define HUB75_HEIGHT        32
#define HUB75_SCAN_ROWS     16      // 1/16 scan - rows y and y + 16 are shifted in together
#define HUB75_ROW_WORDS     (1 + HUB75_WIDTH + 1)
#define HUB75_FRAME_WORDS   (HUB75_SCAN_ROWS * HUB75_ROW_WORDS)

#define HUB75_ADDR_SHIFT    6       // pixel word: bits 0-5 R1 G1 B1 R2 G2 B2, then the row address

// color of the lit pixels - the 1 bit colors combined
#define HUB75_RED           1
#define HUB75_GREEN         2
#define HUB75_BLUE          4

// buf - 4 pages of 128 bytes, bit n of byte x in page p is the pixel (x, p * 8 + n); nullptr = all dark
void hub75BuildFrame(const uint8_t* buf, uint8_t color, uint32_t onCycles, bool mirrorX, uint32_t* frame);

// brightness 0..255 (the contrast of the OLED) -> on time of each row, 1..maxOnCycles + 1 SM cycles
uint32_t hub75OnCycles(uint8_t brightness, uint32_t maxOnCycles);

#endif
