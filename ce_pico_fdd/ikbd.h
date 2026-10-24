#ifndef __IKBD_H__
#define __IKBD_H__

#define IKBD_BAUD_RATE  7812

// tags to distinguish keyboard data from ST commands
#define UARTMARK_STCMD      0xAA
#define UARTMARK_KEYBDATA   0xBB
#define UARTMARK_ALIVE      0xEE

void ikbdInit(void);            // core 0 setup - the uart, the PIO receiver and their IRQs
void ikbdProcessing(void);      // core 0 main loop

#endif
