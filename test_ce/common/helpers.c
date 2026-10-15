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
static void putPadded(const char *str, int len, int width, char padChar, int leftAlign)
{
    int i;

    if(!leftAlign) {
        for(i=len; i<width; i++) {
            Cconout(padChar);
        }
    }

    for(i=0; i<len; i++) {
        Cconout(str[i]);
    }

    if(leftAlign) {
        for(i=len; i<width; i++) {
            Cconout(' ');
        }
    }
}

int ce_printf(const char *format, ...)
{
    static const char digits[] = "0123456789abcdef0123456789ABCDEF";
    va_list args;
    va_start(args, format);

    const char *p;
    for(p = format; *p; p++) {
        if(*p != '%') {
            if(*p == '\n') {            // TOS console needs CR + LF for new line
                Cconout('\r');
            }
            Cconout(*p);
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
                        Cconout('-');
                        width--;
                    } else {
                        tmp[len++] = '-';
                    }
                }

                while(cnt > 0) {
                    tmp[len++] = rev[--cnt];
                }

                putPadded(tmp, len, width, padChar, leftAlign);
                break;
            }

            case 'c':
                tmp[0] = (char) va_arg(args, int);
                putPadded(tmp, 1, width, ' ', leftAlign);
                break;

            case 's': {
                const char *str = va_arg(args, const char *);
                putPadded(str, strlen(str), width, ' ', leftAlign);
                break;
            }

            case '%':
                Cconout('%');
                break;

            case 0:                         // format string ended after '%'
                p--;
                break;

            default:                        // unsupported, just show it
                Cconout('%');
                Cconout(*p);
                break;
        }
    }

    va_end(args);
    return 0;
}
