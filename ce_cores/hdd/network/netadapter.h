#ifndef _NETADAPTER_H_
#define _NETADAPTER_H_

#include <stdint.h>
#include <netinet/in.h>
#include <vector>

#include "readwrapper.h"
#include "icmpwrapper.h"
#include "resolver.h"

// CosmosEx network module (HOSTMOD_NETWORK_ADAPTER) - the host side of the fake STinG (../../../ce_fakesting).
// The STinG calls of the ST come here as ACSI / SCSI commands (netadapter_commands.h), the networking is done with
// the sockets of this host. From ce-atari/ce_main_app/network/netadapter.cpp, made to behave like real STinG 1.26 -
// see ../../../test_ce/test_sting and the reference log in ../../../test_ce/sting_server/reference.

class AcsiDataTrans;

#define NET_HANDLES_COUNT       96
#define NET_STARTING_HANDLE     0x50        // handles the ST gets: 0x50 .. 0xaf - below RES_DIDNT_FINISH_YET and E_* codes

#define network_handleIsValid(X)    ((X) >= NET_STARTING_HANDLE && (X) < (NET_STARTING_HANDLE + NET_HANDLES_COUNT))
#define network_slotToHandle(X)     ((X) + NET_STARTING_HANDLE)
#define network_handleToSlot(X)     ((X) - NET_STARTING_HANDLE)

#define NET_BUFFER_SIZE         (1024 * 1024)

#define LINGER_TIME_WAIT_MS     4000        // after TCP_close() of an open connection the handle stays in TTIME_WAIT
#define LINGER_LAST_ACK_MS      500         // after TCP_close(timeout 0) in TCLOSE_WAIT it stays in TLAST_ACK

class TNetConnection
{
public:
    TNetConnection()  { initVars(); }
    ~TNetConnection() { closeIt(); }

    void closeIt(void);                 // close the sockets and free the slot
    void closeSockets(void);            // close the sockets, the slot stays used (closed / lingering connection)
    void initVars(void);
    bool isFree(void) { return !inUse; }

    bool     inUse;                     // the handle exists for the ST - from open until close (and lingering after it)
    bool     activeNotPassive;          // outgoing or listening
    int      fd;                        // data socket
    int      listenFd;                  // listening socket (passive TCP) until somebody connects
    struct sockaddr_in remote_adr;      // remote address (network byte order)
    uint16_t localPort;
    int      type;                      // TCP / UDP
    int      buff_size;                 // from TCP_open(): the biggest NDB, and half of the send queue limit

    ReadWrapper readWrapper;

    int      bytesInSocket;             // what can be read now
    int      status;                     // TCP state (TESTABLISH, TCLOSE_WAIT, ...)
    int8_t   netError;                  // error for the ST, reported once (E_RRESET, E_REFUSE)
    bool     wasReset;                  // got a RST (or refused) - TCP_close() returns E_RRESET
    uint32_t lingerStart;               // TTIME_WAIT / TLAST_ACK: when it started
    uint32_t lingerMs;

    std::vector<uint8_t> pending;       // TCP_send() while still connecting - sent when connected
};

class NetAdapter
{
public:
    NetAdapter(AcsiDataTrans *dt);
    ~NetAdapter();

    void processCommand(uint8_t *command);      // pCmd: for ICD commands without the 1st byte, as TranslatedDisk gets it
    void closeAndCleanAll(void);

    static uint32_t findLocalIp(void);          // the IP the fake STinG reports as its own

private:
    uint8_t         *cmd;
    AcsiDataTrans   *dataTrans;
    uint8_t         *dataBuffer;
    uint32_t         localIp;
    int              nextSlot;                  // handles are given round robin, as real STinG doesn't reuse them right away

    TNetConnection   cons[NET_HANDLES_COUNT];
    IcmpWrapper      icmpWrapper;
    ResolverRequest  resolver;

    void identify(void);

    void conOpen(void);                 // TCP_open() and UDP_open()
    void conClose(void);                // TCP_close() and UDP_close()
    void conSend(void);                 // TCP_send() and UDP_send()

    void conGetCharBuffer(void);        // CNget_char() - old one, 250 bytes, used count as a byte
    void conPeek(void);                 // CNget_char() / CNgets() line cache - bigger, used count as a word
    void conGetNdb(void);               // CNget_NDB()
    void conGetBlock(void);             // CNget_block()
    void conGetString(void);            // CNgets()
    void conUpdateInfo(void);           // CNbyte_count(), CNgetinfo(), connection states

    void icmpSend(void);
    void icmpGetDgrams(void);

    void resolveStart(void);
    void resolveGetResp(void);

    void conOpen_connect(int slot, bool tcpNotUdp, uint16_t localPort, uint32_t remoteHost, uint16_t remotePort, uint16_t buff_size);
    void conOpen_listen (int slot, bool tcpNotUdp, uint16_t localPort, uint16_t buff_size);

    TNetConnection *connFromHandle(int handle, const char *fn);    // NULL (and status set) on bad / unused handle
    void removeUsedChars(TNetConnection *nc, int charsUsed);

    int  findEmptyConnectionSlot(void);
    void updateCons(void);
    void updateCons_active (int i);
    void updateCons_passive(int i);
    void checkTcpSocket    (TNetConnection *nc);    // reset, FIN, nothing more to read
    void flushPending      (TNetConnection *nc);
    bool sendQueueFull     (TNetConnection *nc, int length);
    bool isOurLocalPort    (int proto, uint16_t port);

    static uint16_t getLocalPort(int sockFd);
    static void     setKeepAliveOptions(int fd);
    static void     setNonBlocking(int fd);
    static int      socketQueuedBytes(int fd);  // sent but not yet acknowledged, -1 if unknown
    static const char *cmdName(uint8_t cmd);
};

#endif
