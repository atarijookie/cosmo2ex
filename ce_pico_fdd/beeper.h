#ifndef __BEEPER_H__
#define __BEEPER_H__

#include <cstdint>

// Beeper on the FDD board (PIN_BEEPER) - a buzzer with its own generator, so it's just switched on / off. It's here and
// not on the front panel, so the beeps work without a panel too.
// Core 1 owns the pin: it clicks on STEP in its ISR and ends all the beeps in its loop - the core 0 loop can be busy for
// seconds (WiFi scan, connecting), so a beep ended there could go on and on.

#define BEEP_STEP_MS            2       // a click per head step, the sound of a real drive
#define BEEP_IMAGE_LOADED_MS    50
#define BEEP_HOST_CONNECTED_MS  100
#define BEEP_HOST_LOST_MS       400

void beeperInit(void);              // core 0, with the other pins - before core 1 runs
void beep(uint16_t ms);             // core 0 - core 1 starts it in beeperLoop(); a running longer beep isn't cut short
void beeperStepClick(void);         // core 1, STEP ISR
void beeperLoop(void);              // core 1 loop - starts the beep from core 0, ends the beeps

#endif
