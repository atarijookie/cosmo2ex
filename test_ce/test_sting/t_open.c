// 0x3xxx - TCP_open() / UDP_open() addressing modes, passive (listening) TCP
#include "stest.h"

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

// local port: exact when we asked for a specific one, otherwise only 'from the dynamic range' is exact
static void recCab(int16 handle, uint32_t lhost, int32_t lport, uint32_t rhost, int32_t rport)
{
    CIB *cib = CNgetinfo(handle);

    rec_x("cib", cib != 0);
    if(!cib) {
        return;
    }

    rec_x("protocol", cib->protocol);
    rec_x("lhost", cib->address.lhost == lhost);
    if(lport > 0) {
        rec_x("lport", cib->address.lport == lport);
    } else {
        rec_x("lportdyn", cib->address.lport >= 1024);
        rec_f("lport", cib->address.lport);
    }
    rec_x("rhost", cib->address.rhost == rhost);
    rec_x("rport", cib->address.rport == rport);
    if(cib->address.rhost != rhost) rec_xh("rhostval", cib->address.rhost);
}

// tell the server (port +20) to connect to our listening socket
static int16 askConnectBack(int16 listenHandle)
{
    CIB *cib = CNgetinfo(listenHandle);
    if(!cib) {
        return -2000;
    }

    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_CONNBK, 1000, TMO(5), &waitRes);
    if(handle < 0) {
        return handle;
    }
    if(waitRes != E_NORMAL) {
        TCP_close(handle, 0, 0);
        return waitRes;
    }

    uint8_t msg[6];
    msg[0] = (uint8_t) (MY_ADDR >> 24);
    msg[1] = (uint8_t) (MY_ADDR >> 16);
    msg[2] = (uint8_t) (MY_ADDR >>  8);
    msg[3] = (uint8_t) (MY_ADDR      );
    msg[4] = (uint8_t) (cib->address.lport >> 8);
    msg[5] = (uint8_t) (cib->address.lport     );

    int16 res = sendAll(handle, 1, msg, 6, TMO(3));
    TCP_close(handle, 0, 0);
    return res;
}

// 1000 B through the echo, recorded as one exact 'echo' value
static void echo1000(int16 handle, uint8_t tcpNotUdp)
{
    int16 res = sendAll(handle, tcpNotUdp, wBuf, 1000, TMO(3));
    rec_x("send", res);
    if(res != E_NORMAL) {
        return;
    }

    memset(rBuf, 0, 1000);
    int32_t got = readBytes(handle, rBuf, 1000, TMO(3));
    rec_x("got", got);
    rec_x("data", got == 1000 && memcmp(rBuf, wBuf, 1000) == 0);
}

// a listening socket: ask server to connect, wait, echo
static void passiveFlow(int16 handle)
{
    rec_x("listencount", CNbyte_count(handle));
    rec_x("state", tcpState(handle));

    rec_x("ask", askConnectBack(handle));
    rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
    rec_x("state2", tcpState(handle));

    CIB *cib = CNgetinfo(handle);
    if(cib) {
        rec_x("rhost2", cib->address.rhost == SERVER_ADDR);
        rec_x("lhost2", cib->address.lhost == MY_ADDR);
    }

    echo1000(handle, 1);
}

static void tcpModes(void)
{
    int16 waitRes, handle;
    CAB cab;

    rec_test(0x3010, "TCP_open(IP, port)");
    handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, TMO(5), &waitRes);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("wait", waitRes);
        recCab(handle, MY_ADDR, 0, SERVER_ADDR, PORT_TCP_ECHO);
        echo1000(handle, 1);
        rec_x("close", TCP_close(handle, 0, 0));
    }

    rec_test(0x3011, "TCP_open(CAB, TCP_ACTIVE), lport 30011");
    memset(&cab, 0, sizeof(cab));
    cab.lport = 30011;
    cab.rhost = SERVER_ADDR;
    cab.rport = PORT_TCP_ECHO;
    handle = TCP_open((uint32) &cab, TCP_ACTIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
        recCab(handle, MY_ADDR, 30011, SERVER_ADDR, PORT_TCP_ECHO);
        echo1000(handle, 1);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3012, "TCP_open(CAB, TCP_ACTIVE), lport 0");
    memset(&cab, 0, sizeof(cab));
    cab.rhost = SERVER_ADDR;
    cab.rport = PORT_TCP_ECHO;
    handle = TCP_open((uint32) &cab, TCP_ACTIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
        recCab(handle, MY_ADDR, 0, SERVER_ADDR, PORT_TCP_ECHO);
        echo1000(handle, 1);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3013, "TCP_open(NULL, TCP_ACTIVE) - listens");
    handle = TCP_open(0, TCP_ACTIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, 0, 0, 0, 0);
        passiveFlow(handle);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3014, "TCP_open(CAB, TCP_PASSIVE), lport 30014");
    memset(&cab, 0, sizeof(cab));
    cab.lport = 30014;
    handle = TCP_open((uint32) &cab, TCP_PASSIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, 0, 30014, 0, 0);
        passiveFlow(handle);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3015, "TCP_open(CAB, TCP_PASSIVE), lport 0");
    memset(&cab, 0, sizeof(cab));
    handle = TCP_open((uint32) &cab, TCP_PASSIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, 0, 0, 0, 0);
        passiveFlow(handle);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3016, "TCP_open(NULL, TCP_PASSIVE)");
    handle = TCP_open(0, TCP_PASSIVE, 0, 2000);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, 0, 0, 0, 0);
        passiveFlow(handle);
        rec_x("close", TCP_close(handle, 0, 0));
    } else {
        rec_x("open", handle);
    }
}

static void udpModes(void)
{
    int16 handle;
    CAB cab;

    rec_test(0x3020, "UDP_open(IP, port)");
    handle = UDP_open(SERVER_ADDR, PORT_UDP_ECHO);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, MY_ADDR, 0, SERVER_ADDR, PORT_UDP_ECHO);
        echo1000(handle, 0);
        rec_x("close", UDP_close(handle));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3021, "UDP_open(CAB, UDP_EXTEND), lport 30021");
    memset(&cab, 0, sizeof(cab));
    cab.lport = 30021;
    cab.rhost = SERVER_ADDR;
    cab.rport = PORT_UDP_ECHO;
    handle = UDP_open((uint32) &cab, UDP_EXTEND);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, MY_ADDR, 30021, SERVER_ADDR, PORT_UDP_ECHO);
        echo1000(handle, 0);
        rec_x("close", UDP_close(handle));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3022, "UDP_open(CAB, UDP_EXTEND), lport 0");
    memset(&cab, 0, sizeof(cab));
    cab.rhost = SERVER_ADDR;
    cab.rport = PORT_UDP_ECHO;
    handle = UDP_open((uint32) &cab, UDP_EXTEND);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        recCab(handle, MY_ADDR, 0, SERVER_ADDR, PORT_UDP_ECHO);
        echo1000(handle, 0);
        rec_x("close", UDP_close(handle));
    } else {
        rec_x("open", handle);
    }

    rec_test(0x3023, "UDP_open twice on lport 30023");
    memset(&cab, 0, sizeof(cab));
    cab.lport = 30023;
    cab.rhost = SERVER_ADDR;
    cab.rport = PORT_UDP_ECHO;
    handle = UDP_open((uint32) &cab, UDP_EXTEND);
    int16 handle2 = UDP_open((uint32) &cab, UDP_EXTEND);
    rec_x("opened", handle >= 0);
    rec_x("opened2", handle2 >= 0);
    if(handle2 < 0) rec_x("open2", handle2);
    if(handle  >= 0) UDP_close(handle);
    if(handle2 >= 0) UDP_close(handle2);
}

static int16 listenOn(uint16 port)
{
    CAB cab;
    memset(&cab, 0, sizeof(cab));
    cab.lport = port;
    return TCP_open((uint32) &cab, TCP_PASSIVE, 0, 2000);
}

// different ways how apps wait for an incoming connection
static void passiveWaiting(void)
{
    int16 handle;

    rec_test(0x3030, "PASSIVE wait using TCP_wait_state");
    handle = listenOn(30030);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("waitshort", TCP_wait_state(handle, TESTABLISH, 1));      // nobody connects yet - timeout
        rec_x("ask", askConnectBack(handle));
        rec_x("wait", TCP_wait_state(handle, TESTABLISH, TMO(5)));
        rec_x("close", TCP_close(handle, 0, 0));
    }

    rec_test(0x3031, "PASSIVE wait using CNbyte_count");
    handle = listenOn(30031);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("listen", CNbyte_count(handle));
        rec_x("ask", askConnectBack(handle));

        uint32_t end = ticks() + TMO(5) * 200;
        int32_t res = E_LISTEN;
        while(ticks() < end) {
            res = CNbyte_count(handle);
            if(res != E_LISTEN) break;
        }
        rec_x("count", res);
        rec_x("close", TCP_close(handle, 0, 0));
    }

    rec_test(0x3032, "PASSIVE wait watching CNgetinfo()->rhost");
    handle = listenOn(30032);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        CIB *cib = CNgetinfo(handle);
        rec_x("cib", cib != 0);
        rec_x("ask", askConnectBack(handle));

        uint8_t changed = 0;
        uint32_t end = ticks() + TMO(5) * 200;
        while(cib && ticks() < end) {
            if(cib->address.rhost != 0) {
                changed = 1;
                break;
            }
        }
        rec_x("changed", changed);
        if(cib) rec_x("rhost", cib->address.rhost == SERVER_ADDR);
        rec_x("close", TCP_close(handle, 0, 0));
    }

    rec_test(0x3033, "LISTEN socket: send, get, ack_wait, info");
    handle = listenOn(30033);
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("send", TCP_send(handle, wBuf, 10));
        rec_x("char", CNget_char(handle));
        rec_x("block", CNget_block(handle, rBuf, 10));
        rec_x("gets", CNgets(handle, (char *) rBuf, 10, '\n'));
        rec_xp("ndb", CNget_NDB(handle));
        rec_x("ackwait", TCP_ack_wait(handle, 100));
        rec_x("state", tcpState(handle));

        rec_test(0x3034, "TCP_PASSIVE twice on the same port");
        int16 handle2 = listenOn(30033);
        rec_x("opened2", handle2 >= 0);
        if(handle2 < 0) rec_x("open2", handle2);
        if(handle2 >= 0) TCP_close(handle2, 0, 0);

        rec_x("close", TCP_close(handle, 0, 0));
    }
}

void testOpen(void)
{
    tcpModes();
    udpModes();
    passiveWaiting();
}
