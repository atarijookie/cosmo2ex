#ifndef SCREEN_H_
#define SCREEN_H_

#include <cstdint>

void screenInit(void);

uint8_t screenLines(void);      // text lines in normal size
uint8_t screenCols(void);       // chars per line in normal size
uint8_t screenCols2(void);      // chars per line in double size

void screenClear(uint32_t now);
void screenSetLine(uint8_t line, uint8_t justify, uint8_t flags, const char* text, uint32_t now);
void screenSetBrightness(uint8_t value, uint32_t now);
void screenSetFlipped(bool flipped, uint32_t now);  // upside down or not - redrawn right away
void screenActivity(uint32_t now);      // key press - restores the brightness after dimming

void screenLoop(uint32_t now);          // redraws, scrolls and dims

#endif
