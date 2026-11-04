// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <stdint.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "dbg.h"

static char logPath[] = "X:\\CE_MEDIA.LOG";
static uint8_t on;

static void write(const char *s, int len)
{
    int32_t h = Fopen(logPath, 1);                  // write only; create it the first time
    if(h < 0) {
        h = Fcreate(logPath, 0);
        if(h < 0) {
            return;
        }
    }
    Fseek(0, (int16_t) h, 2);                       // append
    Fwrite((int16_t) h, len, s);
    Fclose((int16_t) h);
}

void dbgStart(char drive)
{
    int32_t h;
    if(!drive) {
        return;
    }
    logPath[0] = drive;
    h = Fcreate(logPath, 0);                        // a new log each run
    if(h >= 0) {
        Fclose((int16_t) h);
    }
    on = TRUE;
}

void dbgLog(const char *msg)
{
    if(on) {
        write(msg, strlen(msg));
        write("\r\n", 2);
    }
}

void dbgLogNum(const char *msg, uint32_t value)
{
    char line[96];
    int  i, n;
    if(!on) {
        return;
    }
    strncpy(line, msg, 80);
    line[80] = 0;
    n = strlen(line);
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    for(i=7; i>=0; i--) {
        line[n++] = "0123456789abcdef"[(value >> (i * 4)) & 15];
    }
    line[n] = 0;
    dbgLog(line);
}
