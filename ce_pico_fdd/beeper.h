#ifndef __BEEPER_H__
#define __BEEPER_H__

#include <cstdint>

// Beeper on the FDD board (PIN_BEEPER) - a buzzer with its own generator, so it's just switched on / off. It's here and
// not on the front panel, so the beeps work without a panel too.
// Core 1 owns the pin: it clicks on STEP in its ISR and ends all the beeps in its loop - the core 0 loop can be busy for
// seconds (WiFi scan, connecting), so a beep ended there could go on and on.
// The click on a head step is very short and ended by a timer alarm on core 1, not by the loop: with clicks of a few ms a
// seek (steps 2-3 ms apart) would be one long tone - short ones make the hum of a real drive seeking, as when the buzzer
// was on the STEP signal itself. The alarm IRQ takes a few us, the read stream refill has ms of slack.

#define BEEP_STEP_US            100     // a click per head step, the sound of a real drive - tune by ear
#define BEEP_IMAGE_LOADED_MS    50
#define BEEP_HOST_CONNECTED_MS  100
#define BEEP_BUTTON_MS          200     // long press reached - the button can be released
#define BEEP_HOST_LOST_MS       400

void beeperInit(void);              // core 0, with the other pins - before core 1 runs
void beeperCore1Init(void);         // core 1 setup, before the STEP IRQ - the alarm ending the step clicks (IRQ on core 1)
void beep(uint16_t ms);             // core 0 - core 1 starts it in beeperLoop(); a running longer beep isn't cut short
void beeperStepClick(void);         // core 1, STEP ISR - nothing if switched off
void beeperSetStepClick(bool on);   // the click on head step on / off (setting beep on step), on after beeperInit()
void beeperLoop(void);              // core 1 loop - starts the beep from core 0, ends the beeps

#endif
