#ifndef _ICMPWRAPPER_H_
#define _ICMPWRAPPER_H_

#include <stdint.h>
#include <functional>

// ICMP of the network module. What comes in is kept as STinG IP_DGRAMs until the fake STinG fetches them for its
// ICMP handlers. From ce-atari/ce_main_app/network/icmpwrapper.cpp.
//
// Raw ICMP socket when possible (ce_cores runs as root): gets the echo replies and the ICMP errors (port unreachable,
// TTL exceeded, ...) like real STinG does. The host gets all ICMP there, so only what belongs to the ST goes on:
// echo replies with an echo ID the ST used, errors about packets of the ST's sockets / pings.
// Without root: a ping socket (SOCK_DGRAM, if ping_group_range allows it) - only echo replies.

#define STING_DGRAM_MAXSIZE     512     // IP_DGRAM (48 B, Pure C layout) + ICMP message
#define STING_DGRAM_HDR_SIZE    48
#define MAX_STING_DGRAMS        32
#define ICMP_ID_MAP_SIZE        64

class TStingDgram {
public:
    TStingDgram() { clear(); }

    void clear(void) {
        count = 0;
        time  = 0;
    }

    bool isEmpty(void) { return count == 0; }

    uint8_t  data[STING_DGRAM_MAXSIZE];
    uint16_t count;
    uint32_t time;
};

class IcmpWrapper
{
public:
     IcmpWrapper(void);
    ~IcmpWrapper(void);

    void     setLocalIp(uint32_t ip) { localIp = ip; }
    void     setPortCheck(std::function<bool(int proto, uint16_t port)> f) { isOurPort = f; }    // for ICMP errors

    void     open(void);                // raw socket, or a ping socket - when the fake STinG starts
    void     receiveAll(void);
    uint8_t  send(uint32_t destinIP, int icmpType, int icmpCode, uint16_t length, uint8_t *data);    // returns E_*
    void     closeAndClean(void);

    uint32_t calcDataByteCountTotal(void);
    int      calcHowManyDatagramsFitIntoBuffer(int bufferSizeBytes);
    int      getNonEmptyIndex(void);

    TStingDgram dgrams[MAX_STING_DGRAMS];

    static uint16_t checksum(const uint8_t *data, int len);     // internet checksum, result in network byte order

private:
    int      fd;
    bool     raw;                       // raw socket (else ping socket)
    uint32_t localIp;
    uint8_t *recvBfr;
    std::function<bool(int proto, uint16_t port)> isOurPort;

    struct {                            // echo IDs / sequences the ST sent
        uint16_t seq;
        uint16_t id;
        bool     used;
    } idMap[ICMP_ID_MAP_SIZE];
    int      idMapNext;
    uint16_t lastId;

    bool     receive(void);
    bool     isForSt(const uint8_t *icmp, int icmpLen);
    void     storeDgram(uint32_t srcIp, const uint8_t *icmp, int icmpLen);
    void     clearOld(void);
    int      getEmptyIndex(void);
    bool     isOurEchoId(uint16_t id);
    uint16_t idForSequence(uint16_t seq);
};

#endif
