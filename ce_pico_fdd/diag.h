#ifndef __DIAG_H__
#define __DIAG_H__

// Crash and reset diagnostics on the debug UART (GP0) - for finding resets / hangs on the hardware.
// - at boot: the boot count since power on (watchdog scratch 0 - survives every reset except power off), and the reset
//   reasons from POWMAN and the watchdog
// - hard fault (also a panic() without a debugger - its breakpoint becomes a hard fault): the core, the stacked
//   registers (pc = where it crashed) and the fault status, then that core stops - the other one runs on

void diagPrintBootInfo(void);       // core 0 setup, after the debug UART is set up

#endif
