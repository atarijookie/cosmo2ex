#include <cstring>
#include "defs.h"
#include "protocol.h"

LineReceiver::LineReceiver() : len(0), overflow(false)
{
    bfr[0] = 0;
}

bool LineReceiver::feed(char c)
{
    if(c == '\r') {
        return false;
    }

    if(c == '\n') {
        bool ok = !overflow;
        bfr[len] = 0;
        len = 0;
        overflow = false;
        return ok;
    }

    if(len >= MAX_MSG_LEN - 1) {        // no space left for the '\n' - drop the whole line
        overflow = true;
        return false;
    }

    bfr[len++] = c;
    return false;
}

// decimal number with 1 to 5 digits, up to max
static bool parseNumber(const char*& p, uint32_t max, uint32_t& value)
{
    value = 0;
    int digits = 0;

    while(*p >= '0' && *p <= '9') {
        if(++digits > 5) {
            return false;
        }
        value = value * 10 + (*p - '0');
        p++;
    }

    return digits > 0 && value <= max;
}

static bool parseText(const char* p, Command& cmd)
{
    if(*p < '0' || *p >= '0' + MAX_LINES) {
        return false;
    }
    cmd.line = *p++ - '0';

    switch(*p++) {
        case 'L': cmd.justify = JUSTIFY_LEFT;   break;
        case 'C': cmd.justify = JUSTIFY_CENTER; break;
        case 'R': cmd.justify = JUSTIFY_RIGHT;  break;
        default:  return false;
    }

    cmd.flags = 0;
    for(; *p && *p != ':'; p++) {       // unknown flags are ignored - newer FDD with an older panel
        switch(*p) {
            case 'B': cmd.flags |= TEXT_BOLD;     break;
            case 'I': cmd.flags |= TEXT_INVERTED; break;
            case '2': cmd.flags |= TEXT_DOUBLE;   break;
            case 'S': cmd.flags |= TEXT_SCROLL;   break;
        }
    }

    if(*p != ':') {
        return false;
    }

    strncpy(cmd.text, p + 1, MAX_TEXT_LEN);
    cmd.text[MAX_TEXT_LEN] = 0;
    cmd.type = CMD_TEXT;
    return true;
}

bool parseCommand(const char* line, Command& cmd)
{
    cmd.type = CMD_NONE;

    if(strcmp(line, "?") == 0) {
        cmd.type = CMD_IDENTIFY;
        return true;
    }

    if(strcmp(line, "C") == 0) {
        cmd.type = CMD_CLEAR;
        return true;
    }

    if(strcmp(line, "!BOOT") == 0) {
        cmd.type = CMD_BOOTLOADER;
        return true;
    }

    switch(line[0]) {
        case 'T':
            return parseText(line + 1, cmd);

        case 'D': {
            const char* p = line + 1;
            uint32_t value;
            if(!parseNumber(p, 255, value) || *p) {
                return false;
            }
            cmd.brightness = value;
            cmd.type = CMD_BRIGHTNESS;
            return true;
        }
    }

    return false;
}
