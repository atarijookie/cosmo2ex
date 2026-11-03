// 0x7xxx - calls that may crash a STinG: run last, one call per test, so a crash shows which call it was
// (the log file has the header of the crashing test as its last line) and all other results are already saved.
#include "stest.h"

void testRisky(uint8_t crashers)
{
    int16 waitRes, handle;

    rec_test(0x7001, "TCP_send 0 bytes");
    handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("wait", waitRes);
        rec_x("send", TCP_send(handle, wBuf, 0));
        rec_x("close", TCP_close(handle, 0, 0));
    }

    rec_test(0x7002, "UDP_send 0 bytes");
    handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("send", UDP_send(handle, wBuf, 0));
        waitMs(500);
        rec_x("count", CNbyte_count(handle));          // did the empty datagram come back as something?
        rec_x("close", UDP_close(handle));
    }

    rec_test(0x7003, "KRfree(NULL)");
    KRfree(0);
    rec_x("survived", 1);

    if(!crashers) {
        return;
    }

    // Known crashes of STinG 1.26 on TT - only with menu X, so a normal run (A) gets to the end.
    // TCP_send(-1): proto/tcp/tcp.c my_TCP_send() has no check for negative length - bufflen - total < -1 is false,
    // then KRmalloc(-1) + memcpy() of 0xffffffff bytes runs over the whole RAM -> bus error (2 bombs) on TT.
    rec_test(0x7004, "TCP_send -1 bytes");
    handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("wait", waitRes);
        rec_x("send", TCP_send(handle, wBuf, -1));
        rec_x("close", TCP_close(handle, 0, 0));
    }

    // Fragmented reply - STinG 1.26 reassembly() (sting/fragm.c) calls IP_discard(NULL) when the last fragment
    // completes the datagram. IP_discard() then KRfree()s pointers read from the exception vectors at address 0x14 /
    // 0x1a; KRfree() reads 8 bytes before them, which is a bus error (2 bombs) on TT. Seen on TT in test 2015 (1473 B).
    // Keep it the very last test.
    rec_test(0x7005, "UDP echo 1473 + 4000 B (fragmented reply)");
    handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        static const int32_t sizes[2] = { 1473, 4000 };
        char key[8];
        int i;

        for(i=0; i<2; i++) {
            ce_snprintf(key, sizeof(key), "send%d", i);
            rec_x(key, sendAll(handle, 0, wBuf, sizes[i], TMO(3)));

            memset(rBuf, 0, sizes[i]);
            int32_t got = readBytes(handle, rBuf, sizes[i], TMO(3));
            ce_snprintf(key, sizeof(key), "got%d", i);
            rec_x(key, got);
            ce_snprintf(key, sizeof(key), "data%d", i);
            rec_x(key, got == sizes[i] && memcmp(rBuf, wBuf, sizes[i]) == 0);
        }

        rec_x("close", UDP_close(handle));
    }
}
