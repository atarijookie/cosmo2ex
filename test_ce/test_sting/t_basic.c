// 0x1xxx - invalid handles, error texts, TCP buffer limits, resolve
#include "stest.h"

static void invalidHandles(uint16_t base, int16 handle)
{
    char bfr[8];
    char name[64];
    int16 res;
    memset(bfr, 0, sizeof(bfr));

#define HTEST(no, fn)   ce_snprintf(name, sizeof(name), fn " - handle %d", handle); rec_test(base + no, name)

    HTEST(0x01, "TCP_close()");
    rec_x("ret", TCP_close(handle, 0, 0));

    HTEST(0x02, "TCP_send()");
    rec_x("ret", TCP_send(handle, bfr, 4));

    HTEST(0x03, "TCP_wait_state()");
    rec_x("ret", TCP_wait_state(handle, TESTABLISH, 1));

    HTEST(0x04, "TCP_ack_wait()");
    rec_x("ret", TCP_ack_wait(handle, 0));

    HTEST(0x05, "TCP_info()");
    if(tpl->TCP_info) {
        TCPIB info;
        memset(&info, 0, sizeof(info));
        info.request = TCPI_state;
        rec_x("ret", TCP_info(handle, &info));
    } else {
        rec_x("missing", 1);
    }

    HTEST(0x11, "UDP_close()");
    rec_x("ret", UDP_close(handle));

    HTEST(0x12, "UDP_send()");
    rec_x("ret", UDP_send(handle, bfr, 4));

    HTEST(0x21, "CNkick()");
    rec_x("ret", CNkick(handle));

    HTEST(0x22, "CNbyte_count()");
    rec_x("ret", CNbyte_count(handle));

    HTEST(0x23, "CNget_char()");
    rec_x("ret", CNget_char(handle));

    HTEST(0x24, "CNget_NDB()");
    rec_xp("ptr", CNget_NDB(handle));

    HTEST(0x25, "CNget_block()");
    rec_x("ret", CNget_block(handle, bfr, 4));

    HTEST(0x26, "CNgetinfo()");
    rec_xp("ptr", CNgetinfo(handle));

    HTEST(0x27, "CNgets()");
    res = CNgets(handle, bfr, 4, '\n');
    rec_x("ret", res);

#undef HTEST
}

static void errorTexts(void)
{
    char key[8];
    int16 code;

    rec_test(0x10c0, "get_err_text() for 0 .. -33, +5, -100");

    for(code = 0; code >= -33; code--) {
        ce_snprintf(key, sizeof(key), "e%d", code);
        rec_xs(key, get_err_text(code));
    }

    rec_xs("e5",    get_err_text(5));
    rec_xs("e-100", get_err_text(-100));
}

// TCP_send() of more than the buffer size given to TCP_open() is refused
static void bufferLimits(void)
{
    int16 waitRes;

    rec_test(0x1100, "TCP_open(bufsize 2000) + send 1000");
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, 5, &waitRes);
    rec_x("wait", waitRes);
    if(handle < 0) {
        rec_x("open", handle);
        return;
    }
    rec_x("send", sendAll(handle, 1, wBuf, 1000, 1));

    rec_test(0x1101, "TCP send 2000 = bufsize");
    rec_x("send", sendAll(handle, 1, wBuf, 2000, 1));

    rec_test(0x1102, "TCP send 2001 > bufsize");
    rec_x("send", sendAll(handle, 1, wBuf, 2001, 1));

    TCP_close(handle, 0, 0);       // 0 / -1 byte sends are in t_risky.c - they may crash STinG
}

static void oneResolve(uint16_t testNo, const char *testName, const char *host, uint8_t withReal, uint8_t exactAddrs)
{
    char   *real = 0;
    uint32  addrs[8];
    char    key[8];

    rec_test(testNo, testName);
    memset(addrs, 0, sizeof(addrs));

    int16 res = resolve((char *) host, withReal ? &real : 0, addrs, 8);

    if(exactAddrs) {
        rec_x("ret", res);
    } else {                            // DNS of real names changes, only 'found / not found' is exact
        rec_x("found", res > 0);
        rec_f("ret", res);
    }

    int i;
    for(i=0; i<res && i<8; i++) {
        ce_snprintf(key, sizeof(key), "a%d", i);
        if(exactAddrs) {
            rec_xh(key, addrs[i]);
        } else {
            char ip[16];
            ce_snprintf(ip, sizeof(ip), "%x", addrs[i]);
            rec_fs(key, ip);
        }
    }

    if(withReal) {
        rec_x("realset", real != 0);
        if(real) {
            if(exactAddrs && res > 0) rec_xs("real", real);  // after a failed resolve() it's leftover data
            else                      rec_fs("real", real);
            KRfree(real);
        }
    }
}

static void testResolve(void)
{
    char ip[16];
    ce_snprintf(ip, sizeof(ip), "%d.%d.%d.%d", (int) (SERVER_ADDR >> 24) & 0xff, (int) (SERVER_ADDR >> 16) & 0xff,
                (int) (SERVER_ADDR >> 8) & 0xff, (int) SERVER_ADDR & 0xff);

    oneResolve(0x1150, "resolve() dotted IP of server",        ip,                    1, 1);
    oneResolve(0x1151, "resolve() dotted IP, real NULL",       ip,                    0, 1);
    oneResolve(0x1152, "resolve() bad dotted IP 1.2.3.400",    "1.2.3.400",           1, 1);
    oneResolve(0x1153, "resolve() nonexistent.invalid",        "nonexistent.invalid", 1, 1);
    oneResolve(0x1154, "resolve() spaces",                     "   ",                 1, 1);
    oneResolve(0x1155, "resolve() empty string",               "",                    1, 1);
    oneResolve(0x1156, "resolve() example.com",                "example.com",         1, 0);
    oneResolve(0x1157, "resolve() example.com, real NULL",     "example.com",         0, 0);
    oneResolve(0x1158, "resolve() localhost",                  "localhost",           1, 1);
}

void testBasic(void)
{
    invalidHandles(0x1000, 10);         // valid range, but not opened
    invalidHandles(0x1040, -1);
    invalidHandles(0x1080, 200);        // out of range

    errorTexts();
    bufferLimits();
    testResolve();
}
