#ifndef _DBG_H_
#define _DBG_H_

// Debug log (-d): lines appended to X:\CE_MEDIA.LOG on the drive of the played file - opened, written and closed for
// every line, so on a translated drive it's on the host at once and survives a crash / reset of the Atari.
// User mode only (GEMDOS) - not from the VBL routine or an interrupt.

#include <stdint.h>

void dbgStart(char drive);                          // 0 = off
void dbgLog(const char *msg);
void dbgLogNum(const char *msg, uint32_t value);    // msg + value in hex

#endif
