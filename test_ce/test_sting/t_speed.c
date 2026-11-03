// 0x8xxx - throughput, only fuzzy values (except 'all data arrived')
#include "stest.h"

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

static int16 openLines(uint16_t lines)
{
    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_LINES, 3000, TMO(5), &waitRes);

    if(handle < 0 || waitRes != E_NORMAL) {
        rec_x("open", handle < 0 ? handle : waitRes);
        if(handle >= 0) TCP_close(handle, 0, 0);
        return -1;
    }

    uint8_t req[2] = { (uint8_t) (lines >> 8), (uint8_t) lines };
    sendAll(handle, 1, req, 2, TMO(3));
    return handle;
}

static void recSpeed(int32_t bytes, uint32_t start)
{
    uint32_t ms = (ticks() - start) * 5;
    if(ms == 0) ms = 1;

    rec_f("bytes", bytes);
    rec_f("ms", ms);
    rec_f("kBps", (bytes / ms));        // bytes per ms = kB per s
}

// receive until EOF using one of the 3 ways
static void speed(uint16_t testNo, const char *name, uint16_t lines, int how)
{
    rec_test(testNo, name);

    int16 handle = openLines(lines);
    if(handle < 0) {
        return;
    }

    int32_t bytes = 0, gotLines = 0;
    uint32_t start = ticks();
    uint32_t end = start + TMO(60) * 200;

    while(ticks() < end) {
        if(how == 0) {                                  // CNgets
            int16 res = CNgets(handle, (char *) rBuf, 256, '\n');
            if(res >= 0) {
                bytes += res + 1;
                gotLines++;
                if(gotLines == lines) break;
                continue;
            }
            if(res == E_NODATA) continue;
            break;
        }

        int32_t avail = CNbyte_count(handle);
        if(avail == 0 || avail == E_NODATA) continue;
        if(avail < 0) break;                            // E_EOF

        if(how == 1) {                                  // CNget_block
            if(avail > 32000) avail = 32000;
            int16 res = CNget_block(handle, rBuf, (int16) avail);
            if(res > 0) bytes += res;
        } else {                                        // CNget_NDB
            NDB *ndb = CNget_NDB(handle);
            if(ndb) {
                bytes += ndb->len;
                KRfree(ndb->ptr);
                KRfree(ndb);
            }
        }
    }

    recSpeed(bytes, start);
    if(how == 0) rec_x("lines", gotLines);
    TCP_close(handle, 0, 0);
}

static void speedSend(void)
{
    rec_test(0x8010, "TCP send 100 kB to counting peer");

    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_SINK, 4000, TMO(5), &waitRes);
    if(handle < 0 || waitRes != E_NORMAL) {
        rec_x("open", handle < 0 ? handle : waitRes);
        if(handle >= 0) TCP_close(handle, 0, 0);
        return;
    }

    int32_t sent = 0;
    uint32_t start = ticks();
    uint32_t end = start + TMO(60) * 200;
    while(sent < 100000 && ticks() < end) {
        int16 res = TCP_send(handle, wBuf, 2000);
        if(res == E_NORMAL) sent += 2000;
        else if(res != E_OBUFFULL) break;
    }

    int16 result;
    TCP_close(handle, TMO(10), &result);
    recSpeed(sent, start);
    rec_x("report", getReport(SCEN_COUNT));
}

void testSpeed(void)
{
    speed(0x8001, "TCP receive 1000 lines, CNgets",       1000,  0);
    speed(0x8002, "TCP receive 20000 lines, CNget_block", 20000, 1);
    speed(0x8003, "TCP receive 20000 lines, CNget_NDB",   20000, 2);
    speedSend();
}
