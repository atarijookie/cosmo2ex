#ifndef _HELPERS_H_
#define _HELPERS_H_

// Helpers shared by all the sub-tests. Basic stuff (mem*, str*, showInt, showHex*, getTicks,
// sleep, getKey, ...) comes from libacsiscsi, this adds what the tests needed on top of that.

#include <stdint.h>
#include "../../libacsiscsi/global.h"
#include "../../libacsiscsi/stdlib.h"

#ifndef BYTE
    #define BYTE    unsigned char
    #define WORD    uint16_t
    #define DWORD   uint32_t
#endif

void    removeAllWaitingKeys(void);

void    toUpper(const char *src, char *dst);
int     strcmpi(const char * str1, const char * str2);
char *  strcpy_switch_rn(char * destination, const char * source);

// minimal printf() replacement writing through GEMDOS, supports: %d %i %u %x %X %c %s %%,
// '0' and '-' flags, field width, 'l' modifier. '\n' is written out as "\r\n".
int     ce_printf(const char *format, ...);

// same formatting as ce_printf(), but into a buffer (always terminated, '\n' stays '\n'), returns the full length
int     ce_snprintf(char *buf, int size, const char *format, ...);

// Send a line to the CosmosEx host log (shows up as 'ST log: ...' in core_hdd.log). Needs CE_DD with CELG support,
// otherwise does nothing. Don't call it from inside other GEMDOS calls - it does its own ACSI / SCSI transaction.
uint8_t hostLogAvailable(void);
void    hostLog(const char *str);

#endif
