// 0x2xxx - echo, UDP datagrams, CN* receive functions
#include "stest.h"
#include "../common/version.h"

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

static int16 openEcho(uint8_t tcpNotUdp, uint16 bufSize)
{
    int16 waitRes = 1;
    int16 handle;

    if(tcpNotUdp) {
        handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, bufSize, TMO(5), &waitRes);
    } else {
        handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    }

    rec_x("opened", handle >= 0);
    if(handle < 0) {
        rec_x("open", handle);
        return handle;
    }

    if(tcpNotUdp) {
        rec_x("wait", waitRes);
    }
    return handle;
}

// receive count bytes through CNget_NDB(), returns number of bytes, *ndbs gets the number of NDBs
static int32_t readNdbs(int16 handle, uint8_t *dst, int32_t count, int16 timeoutSecs, int32_t *ndbs)
{
    uint32_t end = ticks() + (timeoutSecs * 200);
    int32_t got = 0;
    *ndbs = 0;

    while(got < count && ticks() < end) {
        NDB *ndb = CNget_NDB(handle);
        if(!ndb) {
            continue;
        }

        int32_t len = ndb->len;
        if(got + len > count) {         // more than expected - keep only what fits, count the rest
            memcpy(dst + got, ndb->ndata, count - got);
        } else {
            memcpy(dst + got, ndb->ndata, len);
        }
        got += len;
        (*ndbs)++;

        KRfree(ndb->ptr);
        KRfree(ndb);
    }

    return got;
}

// send the blocks one after another, each one is echoed back and checked
static void echoTest(uint16_t testNo, const char *name, uint8_t tcpNotUdp, const int32_t *blocks, int count, uint8_t useNdb)
{
    char key[16];
    int i;
    int32_t maxBlock = 0;

    rec_test(testNo, name);

    for(i=0; i<count; i++) {
        if(blocks[i] > maxBlock) maxBlock = blocks[i];
    }

    int16 handle = openEcho(tcpNotUdp, (uint16) maxBlock);
    if(handle < 0) {
        return;
    }

    for(i=0; i<count; i++) {
        int16 timeout = TMO(2 + (blocks[i] / 2048));

        ce_snprintf(key, sizeof(key), "send%d", i);
        int16 res = sendAll(handle, tcpNotUdp, wBuf, blocks[i], timeout);
        rec_x(key, res);
        if(res != E_NORMAL) {
            break;
        }

        memset(rBuf, 0, blocks[i]);

        int32_t got, ndbs = 0;
        if(useNdb) {
            got = readNdbs(handle, rBuf, blocks[i], timeout, &ndbs);
        } else {
            got = readBytes(handle, rBuf, blocks[i], timeout);
        }

        ce_snprintf(key, sizeof(key), "got%d", i);
        rec_x(key, got);

        ce_snprintf(key, sizeof(key), "data%d", i);
        rec_x(key, (got == blocks[i]) && memcmp(rBuf, wBuf, blocks[i]) == 0);

        if(useNdb) {
            ce_snprintf(key, sizeof(key), "ndbs%d", i);
            if(tcpNotUdp) rec_f(key, ndbs);     // TCP segmentation differs between stacks
            else          rec_x(key, ndbs);     // UDP: one NDB per datagram
        }
    }

    rec_x("close", tcpNotUdp ? TCP_close(handle, 0, 0) : UDP_close(handle));
}

static void echoTests(void)
{
    static const int32_t b1[] = {32};
    static const int32_t b2[] = {250, 250, 250};
    static const int32_t b3[] = {511, 512, 513};
    static const int32_t b4[] = {1000, 250, 1000};
    static const int32_t b5[] = {500, 1000, 1500, 2000};
    static const int32_t b6[] = {5000, 10000, 16000};
    static const int32_t b7[] = {1400, 1472};           // UDP: up to the max unfragmented payload on ethernet - fragmented
                                                        // replies crash STinG 1.26 reassembly on TT, see t_risky.c 0x7005

    echoTest(0x2001, "TCP echo 32",               1, b1, 1, 0);
    echoTest(0x2002, "TCP echo 3x 250",           1, b2, 3, 0);
    echoTest(0x2003, "TCP echo 511 512 513",      1, b3, 3, 0);
    echoTest(0x2004, "TCP echo 1000 250 1000",    1, b4, 3, 0);
    echoTest(0x2005, "TCP echo 500 .. 2000",      1, b5, 4, 0);
    echoTest(0x2006, "TCP echo 5000 10000 16000", 1, b6, 3, 0);
    echoTest(0x2007, "TCP echo 511 512 513, NDB", 1, b3, 3, 1);
    echoTest(0x2008, "TCP echo 5000 .. 16000, NDB", 1, b6, 3, 1);

    echoTest(0x2011, "UDP echo 32",               0, b1, 1, 0);
    echoTest(0x2012, "UDP echo 3x 250",           0, b2, 3, 0);
    echoTest(0x2013, "UDP echo 511 512 513",      0, b3, 3, 0);
    echoTest(0x2014, "UDP echo 1000 250 1000",    0, b4, 3, 0);
    echoTest(0x2015, "UDP echo 1400 1472",        0, b7, 2, 0);
    echoTest(0x2017, "UDP echo 511 512 513, NDB", 0, b3, 3, 1);
}

// 3 datagrams waiting - how the CN* functions see them
static int16 udp3datagrams(uint16_t testNo, const char *name, int count, int32_t size)
{
    rec_test(testNo, name);

    int16 handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", handle >= 0);
    if(handle < 0) {
        rec_x("open", handle);
        return handle;
    }

    int i;
    for(i=0; i<count; i++) {
        UDP_send(handle, wBuf, (int16) size);
    }

    waitMs(1000);
    rec_x("count", CNbyte_count(handle));
    return handle;
}

static void udpDatagrams(void)
{
    int16 handle, a, b, c;

    handle = udp3datagrams(0x2030, "UDP 3x100: 3x CNget_block(100)", 3, 100);
    if(handle >= 0) {
        a = CNget_block(handle, rBuf, 100);
        b = CNget_block(handle, rBuf + 100, 100);
        c = CNget_block(handle, rBuf + 200, 100);
        rec_x("a", a);
        rec_x("b", b);
        rec_x("c", c);
        rec_x("after", CNbyte_count(handle));
        UDP_close(handle);
    }

    handle = udp3datagrams(0x2031, "UDP 1x300: CNget_block(100), count", 1, 300);
    if(handle >= 0) {
        rec_x("get", CNget_block(handle, rBuf, 100));
        rec_x("after", CNbyte_count(handle));
        rec_x("get2", CNget_block(handle, rBuf + 100, 200));
        rec_x("data", memcmp(rBuf, wBuf, 300) == 0);
        UDP_close(handle);
    }

    handle = udp3datagrams(0x2032, "UDP 3x100: 1x CNget_block(300)", 3, 100);
    if(handle >= 0) {
        rec_x("get", CNget_block(handle, rBuf, 300));
        rec_x("after", CNbyte_count(handle));
        UDP_close(handle);
    }

    handle = udp3datagrams(0x2033, "UDP 3x100: CNget_block(150) x2", 3, 100);
    if(handle >= 0) {
        rec_x("get1", CNget_block(handle, rBuf, 150));
        rec_x("after1", CNbyte_count(handle));
        rec_x("get2", CNget_block(handle, rBuf + 150, 150));
        rec_x("after2", CNbyte_count(handle));
        UDP_close(handle);
    }

    handle = udp3datagrams(0x2034, "UDP 3x100: 4x CNget_NDB", 3, 100);
    if(handle >= 0) {
        int i;
        char key[8];
        for(i=0; i<4; i++) {
            NDB *ndb = CNget_NDB(handle);
            ce_snprintf(key, sizeof(key), "len%d", i);
            rec_x(key, ndb ? ndb->len : -1);
            if(ndb) {
                KRfree(ndb->ptr);
                KRfree(ndb);
            }
        }
        UDP_close(handle);
    }

    handle = udp3datagrams(0x2035, "UDP 3x100: 301x CNget_char", 3, 100);
    if(handle >= 0) {
        int i, bad = 0;
        for(i=0; i<300; i++) {
            int16 ch = CNget_char(handle);
            if(ch != wBuf[i % 100]) bad++;
        }
        rec_x("bad", bad);
        rec_x("last", CNget_char(handle));
        UDP_close(handle);
    }
}

// CNbyte_count() + CNget_char(), checks that the count goes down by one per char
static void countAndChar(uint16_t testNo, const char *name, uint8_t tcpNotUdp)
{
    rec_test(testNo, name);

    int16 handle = openEcho(tcpNotUdp, 2000);
    if(handle < 0) {
        return;
    }

    rec_x("send", sendAll(handle, tcpNotUdp, wBuf, 1000, TMO(3)));

    uint32_t end = ticks() + TMO(3) * 200;
    int32_t idx = 0, mismatch = 0, countBad = 0;

    while(idx < 1000 && ticks() < end) {
        int32_t avail = CNbyte_count(handle);
        int32_t i;

        for(i=0; i<avail; i++) {
            int16 ch = CNget_char(handle);
            if(ch != wBuf[idx]) mismatch++;
            idx++;

            int32_t now = CNbyte_count(handle);
            if(now < avail - i - 1) countBad++;     // bytes not consumed yet must still be counted
        }
    }

    rec_x("got", idx);
    rec_x("mismatch", mismatch);
    rec_x("countbad", countBad);
    rec_x("empty", CNget_char(handle));
    rec_x("emptycount", CNbyte_count(handle));

    closeAny(handle, tcpNotUdp);
}

static void cngets(uint16_t testNo, const char *name, uint8_t tcpNotUdp)
{
    rec_test(testNo, name);

    int16 waitRes = 1, handle;
    if(tcpNotUdp) {
        handle = openTcp(SERVER_ADDR, PORT_TCP_LINES, 3000, TMO(5), &waitRes);
        rec_x("wait", waitRes);
    } else {
        handle = UDP_open(SERVER_ADDR, PORT_UDP_LINES);
    }
    rec_x("opened", handle >= 0);
    if(handle < 0) {
        return;
    }

    rec_x("empty", CNgets(handle, (char *) rBuf, 9, '\n'));

    int lines = tcpNotUdp ? 32 : 12;
    uint8_t req[2] = { 0, (uint8_t) lines };
    rec_x("send", sendAll(handle, tcpNotUdp, req, 2, TMO(3)));

    rec_x("waited", waitForBytes(handle, 200, TMO(3)) >= 200);

    rec_x("small", CNgets(handle, (char *) rBuf, 9, '\n'));      // first line is longer than 9

    int i, good = 0;
    int32_t firstLen = 0;
    uint32_t end = ticks() + TMO(5) * 200;
    for(i=0; i<lines && ticks() < end; ) {
        memset(rBuf, 0, 200);
        int16 res = CNgets(handle, (char *) rBuf, 200, '\n');

        if(res == E_NODATA) {
            continue;
        }

        if(i == 0) {
            firstLen = res;
            rec_xs("line0", (char *) rBuf);
        }

        if(res > 0) {
            int lenFromStr = getIntFromStr((char *) rBuf, 4);
            if(lenFromStr + 3 == strlen((char *) rBuf)) {   // header counts the '\n' that CNgets() removes
                good++;
            }
        } else {
            ce_snprintf((char *) rBuf, 32, "err%d", i);
            rec_x((char *) rBuf, res);
            break;
        }
        i++;
    }

    rec_x("len0", firstLen);
    rec_x("lines", i);
    rec_x("good", good);

    if(tcpNotUdp) {                     // server closes after the lines
        waitMs(500);
        rec_x("eofcount", CNbyte_count(handle));
        rec_x("eofgets", CNgets(handle, (char *) rBuf, 200, '\n'));
    } else {
        rec_x("restcount", CNbyte_count(handle));
    }

    closeAny(handle, tcpNotUdp);
}

static void cngetinfo(uint16_t testNo, const char *name, uint8_t tcpNotUdp)
{
    rec_test(testNo, name);

    int16 waitRes = 1, handle;
    uint16 port = tcpNotUdp ? PORT_TCP_CLOSE : PORT_UDP_CLOSE;

    if(tcpNotUdp) {
        handle = openTcp(SERVER_ADDR, port, 2000, TMO(5), &waitRes);
        rec_x("wait", waitRes);
    } else {
        handle = UDP_open(SERVER_ADDR, port);
    }
    rec_x("opened", handle >= 0);
    if(handle < 0) {
        return;
    }

    CIB *cib = CNgetinfo(handle);
    rec_x("cib", cib != 0);
    if(cib) {
        rec_x("protocol", cib->protocol);
        rec_x("rport", cib->address.rport == port);
        rec_x("rhost", cib->address.rhost == SERVER_ADDR);
        rec_x("lhost", cib->address.lhost == MY_ADDR);
        rec_f("lport", cib->address.lport);
        rec_x("status", cib->status);
    }

    uint8_t closeTime[2] = { 1000 >> 8, 1000 & 0xff };          // server closes after 1000 ms
    rec_x("send", sendAll(handle, tcpNotUdp, closeTime, 2, TMO(3)));

    if(tcpNotUdp) {
        uint32_t start = ticks();
        rec_x("waitclose", TCP_wait_state(handle, TCLOSE_WAIT, TMO(4)));
        rec_f("ms", (ticks() - start) * 5);
        rec_x("state", tcpState(handle));
        rec_x("count", CNbyte_count(handle));
        rec_x("char", CNget_char(handle));
        if(cib) {
            rec_x("status2", cib->status);
        }
    }

    rec_x("close", tcpNotUdp ? TCP_close(handle, 0, 0) : UDP_close(handle));
    rec_xp("cibafter", CNgetinfo(handle));
}

// only CNget_char(), sending more whenever all sent data were received
static void onlyGetChar(void)
{
    rec_test(0x200a, "TCP only CNget_char, 5000 B");

    int16 handle = openEcho(1, 1024);
    if(handle < 0) {
        return;
    }

    int32_t i = 0, sent = 0, mismatch = 0;
    int16 err = 0;
    uint32_t end = ticks() + TMO(15) * 200;

    while(i < 5000 && ticks() < end) {
        int16 ch = CNget_char(handle);

        if(ch >= 0) {
            if(ch != wBuf[i]) mismatch++;
            i++;
        } else if(ch == E_NODATA) {
            if(sent <= i) {
                int32_t len = 1 + (sent % 7) * 150;     // 1 .. 901, same sequence on every run
                if(sent + len > 5000) len = 5000 - sent;

                int16 res = sendAll(handle, 1, wBuf + sent, len, TMO(3));
                if(res != E_NORMAL) {
                    err = res;
                    break;
                }
                sent += len;
            }
        } else {
            err = ch;
            break;
        }
    }

    rec_x("got", i);
    rec_x("mismatch", mismatch);
    rec_x("err", err);
    TCP_close(handle, 0, 0);
}

// CNget_block() must not write outside of the given buffer
static void canaries(void)
{
    static const int16 sizes[] = {511, 512, 513, 1023, 1024, 1025, 1535, 1536, 1537};
    char name[48];
    int i, offset;
    uint16_t testNo = 0x2040;

    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);

    for(i=0; i<9; i++) {
        for(offset=0; offset<2; offset++) {
            ce_snprintf(name, sizeof(name), "CNget_block canaries %d, offset %d", sizes[i], offset);
            rec_test(testNo++, name);

            if(handle < 0) {
                rec_x("open", handle);
                continue;
            }

            int16 size = sizes[i];
            rec_x("send", sendAll(handle, 1, wBuf, size, TMO(3)));
            rec_x("waited", waitForBytes(handle, size, TMO(3)) >= size);

            uint8_t *p = rBuf + offset;
            memcpy(p, "CANARIES", 8);
            memset(p + 8, 0, size);
            memcpy(p + 8 + size, "HMNGBRDS", 8);

            rec_x("get", CNget_block(handle, p + 8, size));
            rec_x("data", memcmp(p + 8, wBuf, size) == 0);
            rec_x("before", memcmp(p, "CANARIES", 8) == 0);
            rec_x("after", memcmp(p + 8 + size, "HMNGBRDS", 8) == 0);
        }
    }

    if(handle >= 0) {
        TCP_close(handle, 0, 0);
    }
}

void testData(void)
{
    echoTests();
    udpDatagrams();

    countAndChar(0x2020, "TCP CNbyte_count + CNget_char", 1);
    countAndChar(0x2021, "UDP CNbyte_count + CNget_char", 0);

    cngets(0x2022, "TCP CNgets", 1);
    cngets(0x2023, "UDP CNgets", 0);

    cngetinfo(0x2024, "TCP CNgetinfo + remote close", 1);
    cngetinfo(0x2025, "UDP CNgetinfo", 0);

    onlyGetChar();
    canaries();
}
