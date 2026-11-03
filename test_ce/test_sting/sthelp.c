#include "stest.h"

// 200 Hz ticks - getTicks() of libacsiscsi reads the system variable directly, so needs supervisor mode
static uint32_t ticks(void)
{
    return getTicksAsUser();
}

void waitMs(int ms)
{
    msleep(ms);
}

void fillWriteBuffer(void)
{
    int32_t i;
    for(i=0; i<BUF_SIZE; i++) {
        wBuf[i] = (uint8_t) i;
    }
}

int16 openTcp(uint32 host, uint16 port, uint16 bufSize, int16 waitSecs, int16 *waitRes)
{
    int16 handle = TCP_open(host, port, 0, bufSize);

    if(waitRes) {
        *waitRes = 1;               // not waited
    }

    if(handle >= 0 && waitSecs > 0) {
        int16 res = TCP_wait_state(handle, TESTABLISH, waitSecs);

        if(waitRes) {
            *waitRes = res;
        }
    }

    return handle;
}

int16 sendAll(int16 handle, uint8_t tcpNotUdp, const void *data, int32_t len, int16 timeoutSecs)
{
    uint32_t end = ticks() + (timeoutSecs * 200);
    int16 res;

    while(1) {
        res = tcpNotUdp ? TCP_send(handle, (void *) data, (int16) len) : UDP_send(handle, (void *) data, (int16) len);

        if(res != E_OBUFFULL) {     // sent or real error
            return res;
        }

        if(ticks() >= end) {        // still full after timeout
            return res;
        }
    }
}

int32_t waitForBytes(int16 handle, int32_t count, int16 timeoutSecs)
{
    uint32_t end = ticks() + (timeoutSecs * 200);
    int32_t res;

    while(1) {
        res = CNbyte_count(handle);

        if(res >= count) {          // got enough
            return res;
        }

        if(res < 0 && res != E_NODATA) {    // error other than 'nothing yet' (E_EOF, E_RRESET, ...)
            return res;
        }

        if(ticks() >= end) {
            return res;
        }
    }
}

int32_t readBytes(int16 handle, uint8_t *dst, int32_t count, int16 timeoutSecs)
{
    uint32_t end = ticks() + (timeoutSecs * 200);
    int32_t got = 0;

    while(got < count) {
        int32_t avail = CNbyte_count(handle);

        if(avail > 0) {
            int32_t want = count - got;
            if(want > avail) want = avail;
            if(want > 32000) want = 32000;

            int16 res = CNget_block(handle, dst + got, (int16) want);

            if(res > 0) {
                got += res;
                continue;
            }

            if(res != E_NODATA) {   // error
                return got ? got : res;
            }
        } else if(avail < 0 && avail != E_NODATA) {     // EOF, reset, ...
            return got ? got : avail;
        }

        if(ticks() >= end) {
            break;
        }
    }

    return got;
}

int16 sendScenario(int16 handle, char cmd, uint16_t arg1, uint32_t arg2)
{
    uint8_t hdr[8];

    hdr[0] = cmd;
    hdr[1] = 0;
    hdr[2] = (uint8_t) (arg1 >>  8);
    hdr[3] = (uint8_t) (arg1      );
    hdr[4] = (uint8_t) (arg2 >> 24);
    hdr[5] = (uint8_t) (arg2 >> 16);
    hdr[6] = (uint8_t) (arg2 >>  8);
    hdr[7] = (uint8_t) (arg2      );

    return sendAll(handle, 1, hdr, 8, 3);
}

// the report line is "<cmd> <count>\n", "- 0\n" while the scenario still runs
int32_t getReport(char expectCmd)
{
    uint32_t end = ticks() + (5 * 200);

    while(ticks() < end) {
        int16 waitRes;
        int16 handle = openTcp(SERVER_ADDR, PORT_TCP_REPORT, 1000, 3, &waitRes);

        if(handle < 0) {
            return -1000;
        }

        char line[32];
        memset(line, 0, sizeof(line));

        int32_t got = 0;
        uint32_t lineEnd = ticks() + (3 * 200);
        while(ticks() < lineEnd && got < (int32_t) sizeof(line) - 1) {
            int16 c = CNget_char(handle);

            if(c >= 0) {
                if(c == '\n') break;
                line[got++] = (char) c;
            } else if(c != E_NODATA) {      // closed before end of line
                break;
            }
        }

        TCP_close(handle, 0, 0);

        if(line[0] == expectCmd && line[1] == ' ') {     // report of the expected scenario
            int32_t count = 0;
            char *p = line + 2;
            while(*p >= '0' && *p <= '9') {
                count = (count * 10) + (*p - '0');
                p++;
            }
            return count;
        }

        waitMs(300);        // still running or other report, try again
    }

    return -1001;
}

int16 tcpState(int16 handle)
{
    if(!tpl->TCP_info) {
        return -100;
    }

    TCPIB info;
    memset(&info, 0, sizeof(info));
    info.request = TCPI_state;

    int16 res = TCP_info(handle, &info);
    if(res < 0) {
        return res;
    }

    return (int16) info.state;
}

void closeAny(int16 handle, uint8_t tcpNotUdp)
{
    if(handle < 0) {
        return;
    }

    if(tcpNotUdp) {
        TCP_close(handle, 0, 0);
    } else {
        UDP_close(handle);
    }
}
