#include <mint/osbind.h>
#include <stdarg.h>

#include "helpers.h"

//--------------------------------------------------
void removeAllWaitingKeys(void)
{
    // read all the possibly waiting keys, so we can ignore them...
    while(Cconis() != 0) {          // something waiting? read it
        Cnecin();
    }
}

//--------------------------------------------------
void toUpper(const char *src, char *dst)
{
    int i, len;
    len = strlen(src);

    for(i=0; i<len; i++) {
        if(src[i] >='a' && src[i]<='z') {       // if it's lower case, switch to upper
            dst[i] = src[i] - 32;
        } else {                                // otherwise just copy
            dst[i] = src[i];
        }
    }

    dst[len] = 0;                               // terminate string
}

int strcmpi(const char * str1, const char * str2)
{
    char tmp1[256], tmp2[256];
    toUpper(str1, tmp1);
    toUpper(str2, tmp2);

    return strcmp(tmp1, tmp2);
}

char *strcpy_switch_rn(char * destination, const char * source)
{
    int i = 0;

    while(1) {
        if(source[i] != '\n' && source[i] != '\r') {    // if it's not \r or \n, just copy it
            destination[i] = source[i];
        } else {                                        // swap \r and \n
            if(source[i] == '\n') {
                destination[i] = '\r';
            } else {
                destination[i] = '\n';
            }
        }

        if(source[i] == 0) {            // terminating zero found?
            break;
        }

        i++;
    }

    return destination;
}

//--------------------------------------------------
// formatted output goes either to console (ce_printf) or to a buffer (ce_snprintf)
typedef struct {
    char *buf;          // NULL = console
    int   size;
    int   len;
} TFmtSink;

static void sinkPut(TFmtSink *s, char c)
{
    if(!s->buf) {                       // console
        if(c == '\n') {                 // TOS console needs CR + LF for new line
            Cconout('\r');
        }
        Cconout(c);
        return;
    }

    if(s->len < s->size - 1) {          // buffer - keep space for terminator
        s->buf[s->len] = c;
    }
    s->len++;
}

static void putPadded(TFmtSink *s, const char *str, int len, int width, char padChar, int leftAlign)
{
    int i;

    if(!leftAlign) {
        for(i=len; i<width; i++) {
            sinkPut(s, padChar);
        }
    }

    for(i=0; i<len; i++) {
        sinkPut(s, str[i]);
    }

    if(leftAlign) {
        for(i=len; i<width; i++) {
            sinkPut(s, ' ');
        }
    }
}

static void ce_vformat(TFmtSink *s, const char *format, va_list args)
{
    static const char digits[] = "0123456789abcdef0123456789ABCDEF";

    const char *p;
    for(p = format; *p; p++) {
        if(*p != '%') {
            sinkPut(s, *p);
            continue;
        }

        p++;                            // skip '%'

        // flags
        char padChar = ' ';
        int leftAlign = 0;
        while(*p == '0' || *p == '-') {
            if(*p == '0') padChar = '0';
            if(*p == '-') leftAlign = 1;
            p++;
        }

        // width
        int width = 0;
        while(*p >= '0' && *p <= '9') {
            width = (width * 10) + (*p - '0');
            p++;
        }

        if(*p == 'l') {                 // long modifier - int is 32 bit anyway
            p++;
        }

        char tmp[16];
        int len = 0;

        switch(*p) {
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X': {
                uint32_t value = va_arg(args, uint32_t);
                int base = (*p == 'x' || *p == 'X') ? 16 : 10;
                int upper = (*p == 'X') ? 16 : 0;
                int negative = 0;

                if((*p == 'd' || *p == 'i') && ((int32_t) value) < 0) {
                    negative = 1;
                    value = -((int32_t) value);
                }

                char rev[12];
                int cnt = 0;
                do {                                // convert to digits, in reversed order
                    rev[cnt++] = digits[upper + (value % base)];
                    value /= base;
                } while(value != 0);

                if(negative) {
                    if(padChar == '0') {            // '-' goes before zero padding
                        sinkPut(s, '-');
                        width--;
                    } else {
                        tmp[len++] = '-';
                    }
                }

                while(cnt > 0) {
                    tmp[len++] = rev[--cnt];
                }

                putPadded(s, tmp, len, width, padChar, leftAlign);
                break;
            }

            case 'c':
                tmp[0] = (char) va_arg(args, int);
                putPadded(s, tmp, 1, width, ' ', leftAlign);
                break;

            case 's': {
                const char *str = va_arg(args, const char *);
                putPadded(s, str, strlen(str), width, ' ', leftAlign);
                break;
            }

            case '%':
                sinkPut(s, '%');
                break;

            case 0:                         // format string ended after '%'
                p--;
                break;

            default:                        // unsupported, just show it
                sinkPut(s, '%');
                sinkPut(s, *p);
                break;
        }
    }
}

int ce_printf(const char *format, ...)
{
    TFmtSink s = { 0, 0, 0 };
    va_list args;
    va_start(args, format);
    ce_vformat(&s, format, args);
    va_end(args);
    return 0;
}

int ce_snprintf(char *buf, int size, const char *format, ...)
{
    TFmtSink s = { buf, size, 0 };
    va_list args;
    va_start(args, format);
    ce_vformat(&s, format, args);
    va_end(args);

    if(size > 0) {
        buf[(s.len < size) ? s.len : (size - 1)] = 0;
    }
    return s.len;
}
