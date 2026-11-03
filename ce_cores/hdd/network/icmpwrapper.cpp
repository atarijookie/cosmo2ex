#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "sting.h"
#include "icmpwrapper.h"

#define RECV_BFR_SIZE       (64 * 1024)
#define ICMP_HDR_SIZE       8           // type, code, checksum, echo id / unused, echo sequence / unused
#define MAX_ICMP_DATA       (8 * 1024)

#ifndef MIN
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#endif

IcmpWrapper::IcmpWrapper(void)
{
    fd          = -1;
    raw         = false;
    localIp     = 0;
    recvBfr     = new uint8_t[RECV_BFR_SIZE];
    idMapNext   = 0;
    lastId      = 0;
    memset(idMap, 0, sizeof(idMap));
}

IcmpWrapper::~IcmpWrapper(void)
{
    closeAndClean();
    delete []recvBfr;
}

void IcmpWrapper::open(void)
{
    if(fd != -1) {
        return;
    }

    fd = Utils::socketCloexec(AF_INET, SOCK_RAW, IPPROTO_ICMP);     // needs root
    raw = (fd != -1);

    if(fd == -1) {                                                  // ping socket - only if ping_group_range allows it
        fd = Utils::socketCloexec(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    }

    if(fd == -1) {
        logHdd(LOG_INFO, "IcmpWrapper::open - no raw or ping socket (%s), the fake STinG can't do ICMP", strerror(errno));
        return;
    }

    logHdd(LOG_DEBUG, "IcmpWrapper::open - using %s socket", raw ? "raw ICMP" : "ping");
}

void IcmpWrapper::closeAndClean(void)
{
    if(fd != -1) {
        close(fd);
        fd = -1;
    }

    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        dgrams[i].clear();
    }

    memset(idMap, 0, sizeof(idMap));
}

uint16_t IcmpWrapper::checksum(const uint8_t *data, int len)
{
    uint32_t sum = 0;

    for(int i=0; i + 1 < len; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }

    if(len & 1) {
        sum += data[len - 1] << 8;
    }

    while(sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return (uint16_t) ~sum;                             // to be stored big endian
}

void IcmpWrapper::clearOld(void)
{
    uint32_t now = Utils::getCurrentMs();

    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        if(!dgrams[i].isEmpty() && (now - dgrams[i].time) >= 10000) {  // not fetched for 10 s - nobody wants it
            dgrams[i].clear();
        }
    }
}

int IcmpWrapper::getEmptyIndex(void)
{
    uint32_t oldestTime = 0xffffffff;
    int oldestIndex = 0;

    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        if(dgrams[i].isEmpty()) {
            return i;
        }

        if(dgrams[i].time < oldestTime) {
            oldestTime  = dgrams[i].time;
            oldestIndex = i;
        }
    }

    dgrams[oldestIndex].clear();                        // full - the oldest one goes
    return oldestIndex;
}

int IcmpWrapper::getNonEmptyIndex(void)
{
    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        if(!dgrams[i].isEmpty()) {
            return i;
        }
    }
    return -1;
}

uint32_t IcmpWrapper::calcDataByteCountTotal(void)
{
    uint32_t sum = 0;

    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        sum += dgrams[i].count;
    }
    return sum;
}

int IcmpWrapper::calcHowManyDatagramsFitIntoBuffer(int bufferSizeBytes)
{
    int gotCount = 0;
    int gotBytes = 2;                                   // the terminating zero word

    for(int i=0; i<MAX_STING_DGRAMS; i++) {
        if(dgrams[i].isEmpty()) {
            continue;
        }

        if(gotBytes + dgrams[i].count + 2 > bufferSizeBytes) {
            break;
        }

        gotCount++;
        gotBytes += 2 + dgrams[i].count;
    }

    return gotCount;
}

void IcmpWrapper::receiveAll(void)
{
    clearOld();

    while(receive()) {
        // all that's waiting
    }
}

bool IcmpWrapper::isOurEchoId(uint16_t id)
{
    for(int i=0; i<ICMP_ID_MAP_SIZE; i++) {
        if(idMap[i].used && idMap[i].id == id) {
            return true;
        }
    }
    return false;
}

uint16_t IcmpWrapper::idForSequence(uint16_t seq)
{
    for(int i=0; i<ICMP_ID_MAP_SIZE; i++) {
        if(idMap[i].used && idMap[i].seq == seq) {
            return idMap[i].id;
        }
    }
    return lastId;
}

// raw socket: is this ICMP message for the ST?
bool IcmpWrapper::isForSt(const uint8_t *icmp, int icmpLen)
{
    int type = icmp[0];

    if(type == 0) {                                     // echo reply - to one of the ST's pings?
        return isOurEchoId(Utils::getWord((uint8_t *) icmp + 4));
    }

    if(type != 3 && type != 4 && type != 5 && type != 11 && type != 12) {  // not an error (echo request, ...)
        return false;
    }

    // error: after the ICMP header comes the IP header + 8 bytes of the packet it is about
    const uint8_t *orig = icmp + ICMP_HDR_SIZE;
    int origLen = icmpLen - ICMP_HDR_SIZE;

    if(origLen < 20 || (orig[0] >> 4) != 4) {
        return false;
    }

    int ihl = (orig[0] & 0x0f) * 4;
    if(ihl < 20 || origLen < ihl + 8) {
        return false;
    }

    if(localIp != 0 && Utils::getDword((uint8_t *) orig + 12) != localIp) {    // not from the IP the ST uses
        return false;
    }

    int proto = orig[9];
    const uint8_t *l4 = orig + ihl;

    if(proto == ICMP) {                                 // about one of the ST's pings
        return isOurEchoId(Utils::getWord((uint8_t *) l4 + 4));
    }

    if((proto == TCP || proto == UDP) && isOurPort) {   // about a packet of one of the ST's sockets
        return isOurPort(proto, Utils::getWord((uint8_t *) l4));    // source port
    }

    return false;
}

bool IcmpWrapper::receive(void)
{
    if(fd == -1) {
        return false;
    }

    struct sockaddr_in src;
    socklen_t srcLen = sizeof(src);

    ssize_t res = recvfrom(fd, recvBfr, RECV_BFR_SIZE, MSG_DONTWAIT, (struct sockaddr *) &src, &srcLen);

    if(res <= 0) {
        if(res < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            logHdd(LOG_ERROR, "IcmpWrapper::receive() - recvfrom() failed: %s", strerror(errno));
        }
        return false;
    }

    uint8_t *icmp = recvBfr;
    int icmpLen = res;

    // raw socket on Linux / macOS, and ping socket on macOS give the IP header too - skip it
    if((icmp[0] >> 4) == 4 && icmpLen >= 20 && (raw || (icmp[0] & 0x0f) >= 5)) {
        int ihl = (icmp[0] & 0x0f) * 4;
        if(ihl >= 20 && ihl < icmpLen) {
            icmp    += ihl;
            icmpLen -= ihl;
        }
    }

    if(icmpLen < ICMP_HDR_SIZE) {
        return true;                                    // nothing useful, but there may be more waiting
    }

    if(raw) {
        if(!isForSt(icmp, icmpLen)) {
            return true;                                // other traffic of the host
        }
    } else if(icmp[0] == 0) {                           // ping socket: the kernel changed the echo ID to its own - put back the ST's one
        Utils::storeWord(icmp + 4, idForSequence(Utils::getWord(icmp + 6)));
        Utils::storeWord(icmp + 2, 0);
        Utils::storeWord(icmp + 2, checksum(icmp, icmpLen));
    }

    storeDgram(ntohl(src.sin_addr.s_addr), icmp, icmpLen);
    return true;
}

void IcmpWrapper::storeDgram(uint32_t srcIp, const uint8_t *icmp, int icmpLen)
{
    TStingDgram *d = &dgrams[getEmptyIndex()];
    d->clear();
    memset(d->data, 0, STING_DGRAM_HDR_SIZE);
    d->time = Utils::getCurrentMs();

    // IP_DGRAM, Pure C layout: IP_HDR (20), options (4), opt_length (2), pkt_data (4), pkt_length (2), timeout (4),
    // ip_gateway (4), recvd (4), next (4) - the fake STinG fixes the pointers
    d->data[0] = 0x45;                                              // IP version 4, header length 5 longs
    Utils::storeWord (d->data +  2, 20 + icmpLen);                  // total length
    d->data[8] = 128;                                               // TTL
    d->data[9] = ICMP;                                              // protocol
    Utils::storeDword(d->data + 12, srcIp);                         // source IP
    Utils::storeDword(d->data + 16, localIp);                       // destination IP
    Utils::storeWord (d->data + 10, checksum(d->data, 20));         // header checksum

    int rest = MIN(STING_DGRAM_MAXSIZE - STING_DGRAM_HDR_SIZE - 2, icmpLen);    // what fits in 512 B
    Utils::storeWord (d->data + 30, rest);                          // pkt_length
    Utils::storeDword(d->data + 32, 128);                           // timeout

    memcpy(d->data + STING_DGRAM_HDR_SIZE, icmp, rest);
    d->count = STING_DGRAM_HDR_SIZE + rest;

    logHdd(LOG_DEBUG, "IcmpWrapper - %d B ICMP type %d code %d for the ST", icmpLen, icmp[0], icmp[1]);
}

uint8_t IcmpWrapper::send(uint32_t destinIP, int icmpType, int icmpCode, uint16_t length, uint8_t *data)
{
    open();

    if(fd == -1) {
        return (uint8_t) E_FNAVAIL;
    }

    // data from ICMP_send() = what comes after type, code and checksum: echo id, sequence, then the payload.
    // Shorter than 4 bytes is OK too (real STinG sends it as it is) - the missing id / sequence are zeros.
    length = MIN(length, MAX_ICMP_DATA);

    uint8_t pkt[ICMP_HDR_SIZE + MAX_ICMP_DATA];
    memset(pkt, 0, ICMP_HDR_SIZE);
    pkt[0] = icmpType;
    pkt[1] = icmpCode;
    memcpy(pkt + 4, data, length);

    int pktLen = 4 + ((length < 4) ? 4 : length);
    Utils::storeWord(pkt + 2, checksum(pkt, pktLen));   // a ping socket on Linux recalculates it, the others don't

    uint16_t id  = Utils::getWord(pkt + 4);
    uint16_t seq = Utils::getWord(pkt + 6);
    lastId = id;
    idMap[idMapNext].seq  = seq;
    idMap[idMapNext].id   = id;
    idMap[idMapNext].used = true;
    idMapNext = (idMapNext + 1) % ICMP_ID_MAP_SIZE;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(destinIP);

    if(sendto(fd, pkt, pktLen, 0, (struct sockaddr *) &dst, sizeof(dst)) == -1) {
        logHdd(LOG_DEBUG, "IcmpWrapper::send() - sendto() failed: %s", strerror(errno));
        // real STinG queues it and doesn't know whether it gets anywhere
    }

    return (uint8_t) E_NORMAL;
}
