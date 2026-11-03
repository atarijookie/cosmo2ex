#ifndef VBL_H
#define VBL_H

extern uint16_t fromVbl;      // if non-zero, then update_con_info() was called from VBL
extern uint16_t vblInstalled; // if non-zero, the VBL Interrupt handler was installed
extern uint16_t vblEnabled;   // set to 0 to disable my VBL execution (other VBL routines will run)
extern volatile uint32_t vblTicks; // counts VBLs once the VBL is installed

void install_vbl(void);   // install vbl handler

#endif
