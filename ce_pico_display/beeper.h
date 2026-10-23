#ifndef BEEPER_H_
#define BEEPER_H_

#include <cstdint>

void beeperInit(bool activeBuzzer);
void beeperStart(uint16_t ms, uint16_t hz, uint32_t now);  // replaces the running beep, ms == 0 stops it
void beeperLoop(uint32_t now);

#endif
