// 0x5xxx - ICMP: handler registration, echo (ping) through ICMP_send + handler, ICMP errors seen by the handler
#include <stddef.h>
#include "stest.h"

#define ICMP_ECHO_REPLY     0
#define ICMP_UNREACHABLE    3
#define ICMP_ECHO           8

#define PING_ID             0xaffe

// STinG hands IP_DGRAM to the handlers, the layout has to match STinG's (Pure C) one
_Static_assert(sizeof(IP_HDR) == 20, "IP_HDR layout");
_Static_assert(offsetof(IP_DGRAM, pkt_data) == 26, "IP_DGRAM layout");
_Static_assert(offsetof(IP_DGRAM, pkt_length) == 30, "IP_DGRAM layout");

// filled by the handlers - they run inside STinG (interrupt time), so only simple stores
static volatile uint16_t echoReplies;       // our echo replies (id PING_ID)
static volatile uint16_t echoBad;           // our id, but bad length / data / source
static volatile uint16_t lastSeq;
static volatile uint16_t otherIcmp;         // not our echo replies
static volatile uint8_t  lastOtherType, lastOtherCode;
static volatile uint16_t peekCalls;         // calls of the handler that doesn't take the datagram

static uint32_t pingDest;

static uint16_t pingData[16];               // what we send after the ICMP header: id, seq, payload
#define PING_LEN    32                      // whole ICMP message: 4 B header (type, code, checksum) + 28 B from pingData

static int16 cdecl echoHandler(IP_DGRAM *dgram)
{
    uint16 *data = (uint16 *) dgram->pkt_data;

    if(dgram->pkt_length < 8) {
        return FALSE;
    }

    if(data[0] != (ICMP_ECHO_REPLY << 8) || data[2] != PING_ID) {   // not our echo reply - not taken
        otherIcmp++;
        lastOtherType = (uint8_t) (data[0] >> 8);
        lastOtherCode = (uint8_t) data[0];
        return FALSE;
    }

    // type+code, checksum, then the echo data (id, seq, payload) as sent
    // id, length, source and payload - not the sequence: a reply may come after the next ping was sent (lastseq has it)
    if(dgram->pkt_length == PING_LEN && dgram->hdr.ip_src == pingDest && memcmp(data + 4, pingData + 2, PING_LEN - 8) == 0) {
        echoReplies++;
    } else {
        echoBad++;
    }
    lastSeq = data[3];

    ICMP_discard(dgram);
    return TRUE;
}

// sees the datagram, but leaves it for the next handler
static int16 cdecl peekHandler(IP_DGRAM *dgram)
{
    (void) dgram;
    peekCalls++;
    return FALSE;
}

// takes everything that isn't an echo reply - to see ICMP errors (unreachable, ...)
static int16 cdecl errorHandler(IP_DGRAM *dgram)
{
    uint16 *data = (uint16 *) dgram->pkt_data;

    if(dgram->pkt_length >= 4 && data[0] != (ICMP_ECHO_REPLY << 8)) {
        otherIcmp++;
        lastOtherType = (uint8_t) (data[0] >> 8);
        lastOtherCode = (uint8_t) data[0];
        ICMP_discard(dgram);
        return TRUE;
    }
    return FALSE;
}

static void resetCounters(void)
{
    echoReplies = echoBad = lastSeq = otherIcmp = peekCalls = 0;
    lastOtherType = lastOtherCode = 0xff;
}

static int16 sendPing(uint32_t dest, uint16_t seq)
{
    int i;
    pingDest    = dest;
    pingData[0] = PING_ID;
    pingData[1] = seq;
    for(i=2; i<16; i++) {
        pingData[i] = 0xa5a5 ^ (i * 0x1111);
    }

    // ICMP_send() data = the part after type, code and checksum
    return ICMP_send(dest, ICMP_ECHO, 0, pingData, PING_LEN - 4);
}

// count pings, record what came back after the wait
static void pings(uint32_t dest, int count, uint8_t exactReplies)
{
    int i;
    int16 res = E_NORMAL, firstErr = E_NORMAL;

    for(i=0; i<count; i++) {
        res = sendPing(dest, (uint16_t) (i + 1));
        if(res != E_NORMAL && firstErr == E_NORMAL) firstErr = res;
        waitMs(200);
    }
    waitMs(1000);

    rec_x("send", firstErr);
    if(exactReplies) rec_x("replies", echoReplies);
    else             rec_f("replies", echoReplies);
    rec_x("bad", echoBad);
    if(echoReplies) rec_x("lastseq", lastSeq);
}

static void registration(void)
{
    rec_test(0x5001, "ICMP_handler(NULL, SET / FORCE / QUERY / REMOVE)");
    rec_x("set",     ICMP_handler(0, HNDLR_SET));
    rec_x("force",   ICMP_handler(0, HNDLR_FORCE));
    rec_x("query",   ICMP_handler(0, HNDLR_QUERY));
    rec_x("remove",  ICMP_handler(0, HNDLR_REMOVE));
    rec_x("query2",  ICMP_handler(0, HNDLR_QUERY));
    rec_x("remove2", ICMP_handler(0, HNDLR_REMOVE));

    ICMP_handler(echoHandler, HNDLR_REMOVE);        // in case an earlier run left it there

    rec_test(0x5002, "ICMP_handler(real) not registered");
    rec_x("remove", ICMP_handler(echoHandler, HNDLR_REMOVE));
    rec_x("query",  ICMP_handler(echoHandler, HNDLR_QUERY));

    rec_test(0x5003, "ICMP_handler(real) SET, SET again, FORCE, QUERY");
    rec_x("set",   ICMP_handler(echoHandler, HNDLR_SET));
    rec_x("set2",  ICMP_handler(echoHandler, HNDLR_SET));
    rec_x("force", ICMP_handler(echoHandler, HNDLR_FORCE));
    rec_x("query", ICMP_handler(echoHandler, HNDLR_QUERY));

    rec_test(0x5004, "ICMP_handler bad flag 7");
    rec_x("ret", ICMP_handler(echoHandler, 7));
}

static void echo(void)
{
    rec_test(0x5010, "5 pings to server");
    resetCounters();
    pings(SERVER_ADDR, 5, 1);
    rec_x("other", otherIcmp);

    rec_test(0x5011, "3 pings to unanswered IP");
    resetCounters();
    pings(UNREACHABLE_IP, 3, 1);

    rec_test(0x5012, "ping with 0 / 1000 bytes data");
    resetCounters();
    rec_x("send0", ICMP_send(SERVER_ADDR, ICMP_ECHO, 0, pingData, 0));
    rec_x("send1000", ICMP_send(SERVER_ADDR, ICMP_ECHO, 0, wBuf, 1000));
    waitMs(1000);
    rec_x("bad", echoBad);              // these don't carry our id / payload layout, so only 'other' or nothing
    rec_f("other", otherIcmp);

    rec_test(0x5013, "2nd handler that doesn't take the reply");
    resetCounters();
    rec_x("set", ICMP_handler(peekHandler, HNDLR_SET));
    pings(SERVER_ADDR, 3, 1);
    rec_x("peek", peekCalls);           // depends on the order STinG calls the handlers in
    rec_x("remove", ICMP_handler(peekHandler, HNDLR_REMOVE));

    rec_test(0x5014, "ICMP_handler REMOVE, then ping");
    rec_x("remove", ICMP_handler(echoHandler, HNDLR_REMOVE));
    rec_x("query",  ICMP_handler(echoHandler, HNDLR_QUERY));
    resetCounters();
    pings(SERVER_ADDR, 3, 1);           // must be 0 - no handler
}

// ICMP errors that STinG gets for our own TCP / UDP traffic - does an app handler see them?
static void errors(void)
{
    rec_test(0x5020, "UDP to closed port, handler sees unreachable?");
    resetCounters();
    rec_x("set", ICMP_handler(errorHandler, HNDLR_SET));

    int16 handle = UDP_open(SERVER_ADDR, PORT_TCP_NONE);    // nothing listens on UDP there either
    rec_x("opened", handle >= 0);
    if(handle >= 0) {
        rec_x("send", UDP_send(handle, wBuf, 10));
        waitMs(1000);
        rec_x("other", otherIcmp);
        if(otherIcmp) {
            rec_x("type", lastOtherType);
            rec_x("code", lastOtherCode);
        }
        rec_x("count", CNbyte_count(handle));       // does the UDP socket report the error itself?
        rec_x("send2", UDP_send(handle, wBuf, 10));
        UDP_close(handle);
    }

    rec_x("remove", ICMP_handler(errorHandler, HNDLR_REMOVE));
}

void testIcmp(void)
{
    registration();
    echo();
    errors();

    ICMP_handler(echoHandler,  HNDLR_REMOVE);       // never leave our handlers in STinG - the app quits after the test
    ICMP_handler(peekHandler,  HNDLR_REMOVE);
    ICMP_handler(errorHandler, HNDLR_REMOVE);
}
