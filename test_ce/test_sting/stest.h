#ifndef _STEST_H_
#define _STEST_H_

// shared stuff of the STinG compatibility test (test_sting)

#include <mint/osbind.h>
#include "../common/helpers.h"
#include "transprt.h"
#include "rec.h"

extern TPL     *tpl;
extern DRV_LIST *stingDrivers;
extern uint32_t SERVER_ADDR;
extern uint32_t MY_ADDR;                // our IP, as STinG reports it in CNgetinfo() of a connected socket
extern uint8_t  slowMachine;            // longer timeouts

extern uint8_t *rBuf, *wBuf;            // wBuf[i] = (uint8_t) i
#define BUF_SIZE        (130 * 1024)

// ports of the test server, see ../sting_server/sting_server.c - all relative to the base port
extern uint16_t serverPortBase;         // 10000 by default, asked at start (server: sting_server -p <port>)
#define PORT_BASE       serverPortBase
#define PORT_TCP_ECHO   (PORT_BASE +  0)    // echo
#define PORT_TCP_BLOCKS (PORT_BASE +  1)    // sends blocks: count, length, pause
#define PORT_TCP_LINES  (PORT_BASE +  2)    // sends N text lines, then closes
#define PORT_TCP_CLOSE  (PORT_BASE +  3)    // closes after N ms
#define PORT_UDP_ECHO   (PORT_BASE +  4)
#define PORT_UDP_BLOCKS (PORT_BASE +  5)
#define PORT_UDP_LINES  (PORT_BASE +  6)
#define PORT_UDP_CLOSE  (PORT_BASE +  7)
#define PORT_TCP_SCEN   (PORT_BASE +  8)    // edge case scenarios, see SCEN_*
#define PORT_TCP_NONE   (PORT_BASE +  9)    // nothing listens here -> connection refused
#define PORT_TCP_REPORT (PORT_BASE + 10)    // sends the report of the last scenario as one line, then closes
#define PORT_TCP_SINK   (PORT_BASE + 11)    // like SCEN_COUNT, but with normal (big) receive window - for speed
#define PORT_TCP_CONNBK (PORT_BASE + 20)    // gets IP + port, connects there, then echoes

// scenarios on PORT_TCP_SCEN - the client sends 8 bytes: cmd, 0, arg1 (16 bit), arg2 (32 bit), big endian
#define SCEN_RESET      'R'     // reset right away
#define SCEN_FIN_DATA   'F'     // wait arg1 ms, send arg2 bytes (wBuf pattern), close
#define SCEN_RST_DATA   'X'     // wait arg1 ms, send arg2 bytes, reset
#define SCEN_HALFCLOSE  'H'     // shutdown(SHUT_WR) right away, count received bytes until EOF, report "H <count>"
#define SCEN_STALL      'S'     // don't read for arg1 ms, then count received bytes until EOF, report "S <count>"
#define SCEN_COUNT      'C'     // count received bytes until EOF, report "C <count>"

// timeouts get longer on slow machines / emulators
#define TMO(secs)       ((int16) (slowMachine ? (secs) * 3 : (secs)))

// IP that is never answered (TEST-NET-1, RFC 5737)
#define UNREACHABLE_IP  0xC0000201

// helpers.c
void    fillWriteBuffer(void);
int16   openTcp(uint32 host, uint16 port, uint16 bufSize, int16 waitSecs, int16 *waitRes);
int16   sendAll(int16 handle, uint8_t tcpNotUdp, const void *data, int32_t len, int16 timeoutSecs);
int32_t waitForBytes(int16 handle, int32_t count, int16 timeoutSecs);   // returns last CNbyte_count()
int32_t readBytes(int16 handle, uint8_t *dst, int32_t count, int16 timeoutSecs);
int16   sendScenario(int16 handle, char cmd, uint16_t arg1, uint32_t arg2);
int32_t getReport(char expectCmd);      // reads the last scenario report from the server, returns count or < 0
int16   tcpState(int16 handle);         // state through TCP_info(), or -100 when TCP_info is missing / fails
void    closeAny(int16 handle, uint8_t tcpNotUdp);
void    waitMs(int ms);

// tests
void testBasic(void);       // 0x1xxx - invalid handles, error texts, buffer limits, resolve
void testData(void);        // 0x2xxx - echo, UDP datagrams, CN* receive functions
void testOpen(void);        // 0x3xxx - TCP_open / UDP_open addressing modes, passive open
void testEdge(void);        // 0x4xxx - refused, reset, FIN with data, half close, flush on close, stalled peer, ...
void testIcmp(void);        // 0x5xxx - ICMP handlers, ping, ICMP errors seen by a handler
void testOnline(void);      // 0x6xxx - carrier_detect, query_port, getvstr / setvstr, get_dftab, KRmalloc
void testRisky(uint8_t crashers); // 0x7xxx - odd calls (0 byte sends, KRfree(NULL)); crashers = also the 2 that crash STinG 1.26
void testSpeed(void);       // 0x8xxx - throughput, fuzzy only

#endif
