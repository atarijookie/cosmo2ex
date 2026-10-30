#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

uint8_t screenVblTick(void);        // called on every VBL, non-zero when screenworker() should be called
void    screenworker(void);         // called from VBL with FLOCK acquired

#endif
