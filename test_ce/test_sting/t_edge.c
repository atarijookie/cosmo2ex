// 0x4xxx - edge cases: refused, reset, FIN with unread data, half close, flush on close, stalled peer, handles, ...
#include "stest.h"

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

// connected socket to the scenario port, the scenario already sent
static int16 openScenario(char cmd, uint16_t arg1, uint32_t arg2, uint16 bufSize)
{
    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_SCEN, bufSize, TMO(5), &waitRes);

    rec_x("opened", handle >= 0);
    if(handle < 0) {
        rec_x("open", handle);
        return handle;
    }
    rec_x("wait", waitRes);

    rec_x("scen", sendScenario(handle, cmd, arg1, arg2));
    return handle;
}

// wait until CNbyte_count() returns an error other than E_NODATA, or timeout
static int32_t waitForError(int16 handle, int16 secs)
{
    uint32_t end = ticks() + secs * 200;
    int32_t res = 0;

    while(ticks() < end) {
        res = CNbyte_count(handle);
        if(res < 0 && res != E_NODATA) {
            break;
        }
    }
    return res;
}

static void refused(void)
{
    rec_test(0x4001, "connect to closed port (RST on SYN)");

    int16 handle = TCP_open(SERVER_ADDR, PORT_TCP_NONE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle < 0) {
        rec_x("open", handle);
        return;
    }

    rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
    rec_x("state", tcpState(handle));
    rec_x("count", CNbyte_count(handle));
    rec_x("send", TCP_send(handle, wBuf, 10));
    rec_x("char", CNget_char(handle));
    rec_x("close", TCP_close(handle, 0, 0));
    rec_x("close2", TCP_close(handle, 0, 0));
}

static void unreachable(void)
{
    rec_test(0x4002, "connect to unanswered IP, wait 3 s");

    int16 handle = TCP_open(UNREACHABLE_IP, 80, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle < 0) {
        rec_x("open", handle);
        return;
    }

    uint32_t start = ticks();
    rec_x("wait", TCP_wait_state(handle, TESTABLISH, 3));
    rec_f("ms", (ticks() - start) * 5);
    rec_x("state", tcpState(handle));
    rec_x("count", CNbyte_count(handle));
    rec_x("send", TCP_send(handle, wBuf, 10));      // queued before connected?
    rec_x("close", TCP_close(handle, 0, 0));

    rec_test(0x4003, "UDP to unanswered IP");
    handle = UDP_open(UNREACHABLE_IP, 9);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("send", UDP_send(handle, wBuf, 100));
        waitMs(500);
        rec_x("count", CNbyte_count(handle));
        rec_x("close", UDP_close(handle));
    }
}

static void resetByPeer(void)
{
    rec_test(0x4010, "peer resets right after connect");
    int16 handle = openScenario(SCEN_RESET, 0, 0, 2000);
    if(handle < 0) return;

    rec_x("err", waitForError(handle, TMO(3)));
    rec_x("count", CNbyte_count(handle));
    rec_x("state", tcpState(handle));
    rec_x("char", CNget_char(handle));
    rec_x("block", CNget_block(handle, rBuf, 10));
    rec_x("send", TCP_send(handle, wBuf, 10));
    rec_x("waitstate", TCP_wait_state(handle, TESTABLISH, 1));
    CIB *cib = CNgetinfo(handle);
    rec_x("cib", cib != 0);
    if(cib) rec_x("status", cib->status);
    rec_x("close", TCP_close(handle, 0, 0));

    rec_test(0x4011, "peer sends 2000 B then resets");
    handle = openScenario(SCEN_RST_DATA, 200, 2000, 4000);
    if(handle < 0) return;

    waitMs(1500);                                  // don't read - data and RST are both in by now
    rec_x("count", CNbyte_count(handle));          // is the unread data still there after the RST?
    rec_x("state", tcpState(handle));
    int32_t got = readBytes(handle, rBuf, 2000, 1);
    rec_x("got", got);
    if(got == 2000) rec_x("data", memcmp(rBuf, wBuf, 2000) == 0);
    rec_x("after", CNbyte_count(handle));
    rec_x("char", CNget_char(handle));
    rec_x("close", TCP_close(handle, 0, 0));
}

static void finWithData(void)
{
    rec_test(0x4020, "peer sends 3000 B then FIN, read later");
    int16 handle = openScenario(SCEN_FIN_DATA, 200, 3000, 4000);
    if(handle < 0) return;

    waitMs(1500);
    rec_x("count", CNbyte_count(handle));
    rec_x("state", tcpState(handle));

    rec_x("block", CNget_block(handle, rBuf, 5000));        // more than there is
    rec_x("count2", CNbyte_count(handle));

    int32_t got = readBytes(handle, rBuf, 3000, TMO(3));
    rec_x("got", got);
    rec_x("data", got == 3000 && memcmp(rBuf, wBuf, 3000) == 0);

    rec_x("eofcount", CNbyte_count(handle));
    rec_x("eofchar", CNget_char(handle));
    rec_x("eofblock", CNget_block(handle, rBuf, 10));
    rec_x("eofgets", CNgets(handle, (char *) rBuf, 10, '\n'));
    rec_xp("eofndb", CNget_NDB(handle));
    rec_x("send", TCP_send(handle, wBuf, 10));              // allowed in CLOSE_WAIT
    rec_x("close", TCP_close(handle, 0, 0));

    rec_test(0x4021, "peer sends 100 B then FIN, read with CNgets");
    handle = openScenario(SCEN_FIN_DATA, 0, 100, 2000);     // wBuf pattern has no '\n' (10) before byte 10
    if(handle < 0) return;

    waitMs(1000);
    rec_x("count", CNbyte_count(handle));
    memset(rBuf, 0, 200);
    rec_x("gets1", CNgets(handle, (char *) rBuf, 200, '\n'));   // bytes 0..9, '\n' is byte 10
    rec_x("gets2", CNgets(handle, (char *) rBuf, 200, '\n'));   // 11..99 without '\n', then EOF
    rec_x("count2", CNbyte_count(handle));
    rec_x("close", TCP_close(handle, 0, 0));
}

static void halfClose(void)
{
    rec_test(0x4030, "peer half-closes, we keep sending");
    int16 handle = openScenario(SCEN_HALFCLOSE, 0, 0, 2000);
    if(handle < 0) return;

    rec_x("waitcw", TCP_wait_state(handle, TCLOSE_WAIT, TMO(3)));
    rec_x("state", tcpState(handle));
    rec_x("count", CNbyte_count(handle));

    int i;
    int16 res = E_NORMAL;
    for(i=0; i<5 && res == E_NORMAL; i++) {
        res = sendAll(handle, 1, wBuf, 1000, TMO(3));
    }
    rec_x("send", res);
    rec_x("sent", i);

    int16 result = 1234;
    rec_x("close", TCP_close(handle, TMO(5), &result));
    rec_x("result", result);
    rec_x("report", getReport(SCEN_HALFCLOSE));         // bytes the server got
}

// is the data still in the send queue delivered after TCP_close()?
static void flushOnClose(uint16_t testNo, const char *name, int16 closeTimeout)
{
    rec_test(testNo, name);
    int16 handle = openScenario(SCEN_COUNT, 0, 0, 2000);
    if(handle < 0) return;

    int i;
    int16 res = E_NORMAL;
    for(i=0; i<5 && res == E_NORMAL; i++) {             // 5 x 1000 B, without waiting for acks
        res = sendAll(handle, 1, wBuf, 1000, TMO(3));
    }
    rec_x("send", res);

    int16 result = 1234;
    uint32_t start = ticks();
    rec_x("close", TCP_close(handle, closeTimeout, &result));
    rec_f("ms", (ticks() - start) * 5);
    if(closeTimeout > 0) rec_x("result", result);
    else                 rec_f("result", result);       // async close - value not defined yet
    rec_x("report", getReport(SCEN_COUNT));
    rec_x("closeagain", TCP_close(handle, 0, 0));
}

// peer doesn't read for 3 s - when does TCP_send() start to refuse data?
static void stalledPeer(void)
{
    rec_test(0x4040, "peer stops reading for 3 s");
    int16 handle = openScenario(SCEN_STALL, 3000, 0, 2000);
    if(handle < 0) return;

    int32_t sent = 0;
    int16 res = E_NORMAL;
    uint32_t end = ticks() + 2 * 200;

    while(ticks() < end && sent < 100000) {             // send until it refuses
        res = TCP_send(handle, wBuf, 1000);
        if(res == E_NORMAL) {
            sent += 1000;
        } else if(res != E_OBUFFULL) {
            break;
        }
    }
    rec_x("res", res);              // E_OBUFFULL expected
    rec_f("sentbeforefull", sent);

    int32_t total = sent;
    end = ticks() + TMO(10) * 200;
    while(ticks() < end && total < sent + 20000) {      // peer reads again after 3 s
        res = TCP_send(handle, wBuf, 1000);
        if(res == E_NORMAL) {
            total += 1000;
        } else if(res != E_OBUFFULL) {
            break;
        }
    }
    rec_x("res2", res);
    rec_x("more", total - sent);

    int16 result = 1234;
    rec_x("close", TCP_close(handle, TMO(10), &result));
    rec_x("result", result);
    int32_t report = getReport(SCEN_STALL);
    rec_x("reportok", report == total);
    rec_f("report", report);
}

static void receiveCorners(void)
{
    int16 waitRes;

    rec_test(0x4050, "CNget_block more than available");
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);
    rec_x("opened", handle >= 0);
    if(handle < 0) return;
    rec_x("wait", waitRes);

    rec_x("emptychar", CNget_char(handle));
    rec_x("emptyblock", CNget_block(handle, rBuf, 10));
    rec_x("emptycount", CNbyte_count(handle));
    rec_xp("emptyndb", CNget_NDB(handle));
    rec_x("emptygets", CNgets(handle, (char *) rBuf, 10, '\n'));

    sendAll(handle, 1, wBuf, 100, TMO(3));
    rec_x("waited", waitForBytes(handle, 100, TMO(3)));
    rec_x("block200", CNget_block(handle, rBuf, 200));
    rec_x("count", CNbyte_count(handle));
    rec_x("block0", CNget_block(handle, rBuf, 0));
    rec_x("count2", CNbyte_count(handle));
    rec_x("block100", CNget_block(handle, rBuf, 100));
    rec_x("data", memcmp(rBuf, wBuf, 100) == 0);

    rec_test(0x4051, "CNgets without delimiter, then with it");
    sendAll(handle, 1, "abc", 3, TMO(3));
    rec_x("waited", waitForBytes(handle, 3, TMO(3)));
    memset(rBuf, 0, 64);
    rec_x("gets", CNgets(handle, (char *) rBuf, 64, '\n'));
    rec_x("count", CNbyte_count(handle));
    sendAll(handle, 1, "def\r\n", 5, TMO(3));
    rec_x("waited2", waitForBytes(handle, 8, TMO(3)));
    memset(rBuf, 0, 64);
    rec_x("gets2", CNgets(handle, (char *) rBuf, 64, '\n'));
    rec_xs("str", (char *) rBuf);
    rec_x("count2", CNbyte_count(handle));

    rec_test(0x4052, "CNgets buffer size vs line length");
    sendAll(handle, 1, "1234\n12345\n", 11, TMO(3));
    rec_x("waited", waitForBytes(handle, 11, TMO(3)));
    memset(rBuf, 0, 64);
    rec_x("gets5", CNgets(handle, (char *) rBuf, 5, '\n'));     // "1234" + terminator fits 5?
    rec_xs("str", (char *) rBuf);
    memset(rBuf, 0, 64);
    rec_x("gets5b", CNgets(handle, (char *) rBuf, 5, '\n'));    // "12345" doesn't
    rec_x("count", CNbyte_count(handle));
    rec_x("gets6", CNgets(handle, (char *) rBuf, 6, '\n'));
    rec_xs("str2", (char *) rBuf);
    rec_x("count2", CNbyte_count(handle));

    rec_test(0x4053, "CNgets with delimiter '\\r'");
    sendAll(handle, 1, "xy\rz\r", 5, TMO(3));
    rec_x("waited", waitForBytes(handle, 5, TMO(3)));
    memset(rBuf, 0, 64);
    rec_x("gets", CNgets(handle, (char *) rBuf, 64, '\r'));
    rec_xs("str", (char *) rBuf);
    rec_x("count", CNbyte_count(handle));
    rec_x("gets2", CNgets(handle, (char *) rBuf, 64, '\r'));
    rec_xs("str2", (char *) rBuf);

    rec_test(0x4054, "CNkick, TCP_ack_wait on idle connection");
    rec_x("kick", CNkick(handle));
    rec_x("ackwait", TCP_ack_wait(handle, 1000));
    rec_x("state", tcpState(handle));

    rec_test(0x4055, "UDP functions on TCP handle");
    rec_x("udpsend", UDP_send(handle, wBuf, 10));
    rec_x("udpclose", UDP_close(handle));
    rec_x("state", tcpState(handle));       // still alive after UDP_close() on it?

    rec_x("close", TCP_close(handle, 0, 0));

    rec_test(0x4056, "TCP functions on UDP handle");
    handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("tcpsend", TCP_send(handle, wBuf, 10));
        rec_x("waitstate", TCP_wait_state(handle, TESTABLISH, 1));
        rec_x("ackwait", TCP_ack_wait(handle, 100));
        rec_x("info", tcpState(handle));
        rec_x("tcpclose", TCP_close(handle, 0, 0));
        rec_x("udpclose", UDP_close(handle));
    }
}

static void lifecycle(void)
{
    rec_test(0x4060, "TCP states through the connection life");

    int16 handle = TCP_open(SERVER_ADDR, PORT_TCP_CLOSE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle < 0) return;

    rec_f("state0", tcpState(handle));                  // SYN_SENT or already ESTABLISHED
    rec_f("count0", CNbyte_count(handle));
    rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
    rec_x("state1", tcpState(handle));

    uint8_t closeTime[2] = { 0, 200 };                  // server closes after 200 ms
    sendAll(handle, 1, closeTime, 2, TMO(3));
    rec_x("waitcw", TCP_wait_state(handle, TCLOSE_WAIT, TMO(3)));
    rec_x("state2", tcpState(handle));

    int16 result = 1234;
    rec_x("close", TCP_close(handle, TMO(3), &result));
    rec_x("result", result);
    rec_x("state3", tcpState(handle));
    rec_x("count3", CNbyte_count(handle));
    rec_xp("cib3", CNgetinfo(handle));

    rec_test(0x4061, "we close first, then wait states");
    int16 waitRes;
    handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);
    rec_x("opened", handle >= 0);
    if(handle < 0) return;
    rec_x("wait", waitRes);

    result = 1234;
    rec_x("close", TCP_close(handle, 0, &result));      // async
    rec_f("result", result);
    rec_x("state", tcpState(handle));
    rec_x("send", TCP_send(handle, wBuf, 10));
    rec_x("count", CNbyte_count(handle));
    waitMs(1000);
    rec_x("state2", tcpState(handle));
    rec_x("closeagain", TCP_close(handle, 0, 0));
}

static void handles(void)
{
    #define MAX_HANDLES 80
    int16 h[MAX_HANDLES];
    int i, count = 0;
    int16 err = 0;
    char key[8];

    rec_test(0x4070, "open UDP handles until it fails");
    for(i=0; i<MAX_HANDLES; i++) {
        h[i] = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
        if(h[i] < 0) {
            err = h[i];
            break;
        }
        count++;
    }
    rec_x("failed", count < MAX_HANDLES);
    if(count < MAX_HANDLES) rec_x("err", err);
    rec_f("count", count);
    for(i=0; i<3 && i<count; i++) {
        ce_snprintf(key, sizeof(key), "h%d", i);
        rec_f(key, h[i]);
    }

    for(i=0; i<count; i++) {
        UDP_close(h[i]);
    }

    rec_test(0x4071, "handles reused after close");
    int16 a = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", a >= 0);
    if(count > 0) rec_x("same", a == h[0]);              // lowest free handle again?
    int16 b = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    UDP_close(a);
    int16 c = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("reuse", c == a);
    UDP_close(b);
    UDP_close(c);
}

void testEdge(void)
{
    refused();
    unreachable();
    resetByPeer();
    finWithData();
    halfClose();
    flushOnClose(0x4031, "5000 B sent, TCP_close(timeout 0)", 0);
    flushOnClose(0x4032, "5000 B sent, TCP_close(timeout 5)", TMO(5));
    stalledPeer();
    receiveCorners();
    lifecycle();
    handles();
}
