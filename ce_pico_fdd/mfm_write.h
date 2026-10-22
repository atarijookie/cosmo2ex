#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/util/queue.h"

#include "defs.h"

// Pulse lengths as counted by mfm_write.pio: 1 count = 0.1 us (20 MHz, 2 instructions per loop), the first ~1.6 us of each
// pulse is cut away by the PIO wait - so duration = count * 0.1 us + ~1.6 us.
// exact expected pulses would be: 23 ticks, 43 ticks, 63 ticks (4 us, 6 us, 8 us), limits below are about 2.9 / 4.9 / 6.9 / 8.9 us
#define PULSE_TOO_SHORT 13
#define PULSE_4US       33
#define PULSE_6US       53
#define PULSE_8US       73

void pio_mfm_write_setup(void);
void mfmWriteCaptureStart(void);
void mfmWriteCaptureEnd(void);

void storeWrittenSectorDataToTrackLocally(uint8_t side, uint8_t track, uint8_t sector, uint8_t* bfr, int len);
