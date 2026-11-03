#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#ifdef __linux__
#include <linux/sockios.h>
#endif
#ifdef __APPLE__
#include <netinet/tcp_fsm.h>
#endif

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../acsidatatrans.h"

#include "sting.h"
#include "netadapter_commands.h"
#include "netadapter.h"

#define SEND_WAIT_MS        2000    // a TCP send waits this long for room in the socket buffer

//--------------------------------------------------------

void TNetConnection::initVars(void)
{
    inUse               = false;
    activeNotPassive    = true;
    fd                  = -1;
    listenFd            = -1;
    type                = 0;
    buff_size           = 0;
    bytesInSocket       = 0;
    status              = TCLOSED;
    netError            = 0;
    wasReset            = false;
    lingerStart         = 0;
    lingerMs            = 0;
    localPort           = 0;
    memset(&remote_adr, 0, sizeof(remote_adr));
    readWrapper.init(-1, 0, 0);
    pending.clear();
}

void TNetConnection::closeSockets(void)
{
    if(fd != -1) {
        close(fd);
        fd = -1;
    }

    if(listenFd != -1) {
        close(listenFd);
        listenFd = -1;
    }

    bytesInSocket = 0;
    readWrapper.init(-1, 0, 0);
    pending.clear();
}

void TNetConnection::closeIt(void)
{
    closeSockets();
    initVars();
}

//--------------------------------------------------------

NetAdapter::NetAdapter(AcsiDataTrans *dt)
{
    cmd         = NULL;
    dataTrans   = dt;
    dataBuffer  = new uint8_t[NET_BUFFER_SIZE];
    localIp     = 0;
    nextSlot    = 0;

    icmpWrapper.setPortCheck([this](int proto, uint16_t port) { return isOurLocalPort(proto, port); });
}

NetAdapter::~NetAdapter()
{
    closeAndCleanAll();
    delete []dataBuffer;
}

void NetAdapter::processCommand(uint8_t *command)
{
    cmd = command;

    if(!dataTrans) {
        logHdd(LOG_ERROR, "NetAdapter::processCommand - no dataTrans");
        return;
    }

    dataTrans->clear();

    logHdd(LOG_DEBUG, "NetAdapter::processCommand - %s", cmdName(cmd[4]));

    switch(cmd[4]) {
        case NET_CMD_IDENTIFY:              identify();         break;

        case NET_CMD_TCP_OPEN:
        case NET_CMD_UDP_OPEN:              conOpen();          break;

        case NET_CMD_TCP_CLOSE:
        case NET_CMD_UDP_CLOSE:             conClose();         break;

        case NET_CMD_TCP_SEND:
        case NET_CMD_UDP_SEND:              conSend();          break;

        case NET_CMD_ICMP_SEND_EVEN:
        case NET_CMD_ICMP_SEND_ODD:         icmpSend();         break;
        case NET_CMD_ICMP_GET_DGRAMS:       icmpGetDgrams();    break;

        case NET_CMD_CNGET_CHAR:            conGetCharBuffer(); break;
        case NET_CMD_CN_PEEK:               conPeek();          break;
        case NET_CMD_CNGET_NDB:             conGetNdb();        break;
        case NET_CMD_CNGET_BLOCK:           conGetBlock();      break;
        case NET_CMD_CNGETS:                conGetString();     break;
        case NET_CMD_CN_UPDATE_INFO:        conUpdateInfo();    break;

        case NET_CMD_RESOLVE:               resolveStart();     break;
        case NET_CMD_RESOLVE_GET_RESPONSE:  resolveGetResp();   break;

        default:                            // TCP_WAIT_STATE, CNKICK, ON_PORT, ... - done on the ST, not used here
            dataTrans->setStatus(E_NORMAL);
            break;
    }

    dataTrans->sendDataAndStatus();
}

//----------------------------------------------
// The IP of the interface the default route goes through - without sending anything, a connected UDP socket just
// picks the route. No default route? The first IPv4 interface that is up and not loopback.
uint32_t NetAdapter::findLocalIp(void)
{
    uint32_t ip = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd >= 0) {
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(53);
        dst.sin_addr.s_addr = htonl(0x08080808);       // any public IP, nothing is sent

        if(connect(fd, (struct sockaddr *) &dst, sizeof(dst)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if(getsockname(fd, (struct sockaddr *) &local, &len) == 0) {
                ip = ntohl(local.sin_addr.s_addr);
            }
        }
        close(fd);
    }

    if(ip != 0) {
        return ip;
    }

    struct ifaddrs *ifaddr;
    if(getifaddrs(&ifaddr) == 0) {
        for(struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

            ip = ntohl(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr.s_addr);
            break;
        }
        freeifaddrs(ifaddr);
    }

    return ip;
}

void NetAdapter::identify(void)
{
    closeAndCleanAll();                             // the fake STinG just started - nothing from its previous run stays

    dataTrans->addDataBfr("CosmosEx network module", 24, false);  // 23 chars + terminator

    uint8_t pad[8];
    memset(pad, 0, sizeof(pad));
    dataTrans->addDataBfr(pad, 8, false);           // config starts at offset 32

    localIp = findLocalIp();
    icmpWrapper.setLocalIp(localIp);
    icmpWrapper.open();                             // now, so ICMP errors for the first packets get caught too

    dataTrans->addDataWord(REQUIRED_NETADAPTER_VERSION);    // 32
    dataTrans->addDataDword(localIp);                       // 34
    dataTrans->addDataWord(NET_HANDLES_COUNT);              // 38

    logHdd(LOG_INFO, "NetAdapter::identify - fake STinG connected, local IP %d.%d.%d.%d",
           (localIp >> 24) & 0xff, (localIp >> 16) & 0xff, (localIp >> 8) & 0xff, localIp & 0xff);

    dataTrans->padDataToMul16();
    dataTrans->setStatus(E_NORMAL);
}

void NetAdapter::closeAndCleanAll(void)
{
    for(int i=0; i<NET_HANDLES_COUNT; i++) {
        cons[i].closeIt();
    }

    icmpWrapper.closeAndClean();
    nextSlot = 0;
}

//----------------------------------------------
TNetConnection *NetAdapter::connFromHandle(int handle, const char *fn)
{
    if(!network_handleIsValid(handle) || cons[network_handleToSlot(handle)].isFree()) {
        logHdd(LOG_DEBUG, "NetAdapter::%s - bad handle: %d", fn, handle);
        dataTrans->setStatus(E_BADHANDLE);
        return NULL;
    }

    return &cons[network_handleToSlot(handle)];
}

void NetAdapter::removeUsedChars(TNetConnection *nc, int charsUsed)
{
    if(charsUsed > 0) {                             // CNget_char() on the ST used this many of the bytes it got last time
        nc->readWrapper.removeBlock(charsUsed);
    }
}

void NetAdapter::setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void NetAdapter::setKeepAliveOptions(int fd)
{
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

#if defined(TCP_KEEPIDLE)                           // Linux, Cygwin
    int idle = 30;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)                        // macOS
    int idle = 30;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif

#if defined(TCP_KEEPCNT) && defined(TCP_KEEPINTVL)
    int cnt = 5, intvl = 60;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
}

uint16_t NetAdapter::getLocalPort(int sockFd)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);

    if(getsockname(sockFd, (struct sockaddr *) &addr, &len) == -1) {
        return 0;
    }

    return ntohs(addr.sin_port);
}

int NetAdapter::socketQueuedBytes(int fd)
{
#if defined(SIOCOUTQ)                               // Linux: not sent + not acknowledged
    int value = 0;
    if(ioctl(fd, SIOCOUTQ, &value) == 0) {
        return value;
    }
#elif defined(SO_NWRITE)                            // macOS
    int value = 0;
    socklen_t len = sizeof(value);
    if(getsockopt(fd, SOL_SOCKET, SO_NWRITE, &value, &len) == 0) {
        return value;
    }
#else
    (void) fd;
#endif
    return -1;
}

// Real STinG has at most about 2x the TCP_open() buffer size unacknowledged, then TCP_send() gives E_OBUFFULL.
// The host socket buffers are much bigger - check against that limit ourselves.
bool NetAdapter::sendQueueFull(TNetConnection *nc, int length)
{
    int limit  = 2 * ((nc->buff_size > 0) ? nc->buff_size : 2048);
    int queued = (nc->status == TSYN_SENT) ? (int) nc->pending.size() : socketQueuedBytes(nc->fd);

    if(queued < 0) {                                // can't find out - let the socket buffer decide
        return false;
    }

    return queued > 0 && queued + length > limit;   // something always fits into an empty queue
}

bool NetAdapter::isOurLocalPort(int proto, uint16_t port)
{
    for(int i=0; i<NET_HANDLES_COUNT; i++) {
        if(!cons[i].isFree() && cons[i].type == proto && cons[i].localPort == port) {
            return true;
        }
    }
    return false;
}

int NetAdapter::findEmptyConnectionSlot(void)
{
    for(int n=0; n<NET_HANDLES_COUNT; n++) {        // round robin - a handle isn't given again right after its close
        int i = (nextSlot + n) % NET_HANDLES_COUNT;

        if(cons[i].isFree()) {
            nextSlot = (i + 1) % NET_HANDLES_COUNT;
            return i;
        }
    }
    return -1;
}

//----------------------------------------------
void NetAdapter::conOpen(void)
{
    bool tcpNotUdp = (cmd[4] == NET_CMD_TCP_OPEN);

    if(!dataTrans->recvData(dataBuffer, 512)) {
        logHdd(LOG_DEBUG, "NetAdapter::conOpen - failed to receive data");
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    updateCons();                                   // lingering handles that expired get free

    int slot = findEmptyConnectionSlot();
    if(slot == -1) {
        logHdd(LOG_DEBUG, "NetAdapter::conOpen - no free handles");
        dataTrans->setStatus(E_NOCCB);
        return;
    }

    uint32_t remoteHost = Utils::getDword(dataBuffer);
    uint16_t remotePort = Utils::getWord (dataBuffer + 4);
    uint16_t buff_size  = Utils::getWord (dataBuffer + 8);     // tos at +6 isn't used
    uint16_t localPort  = Utils::getWord (dataBuffer + 14);

    logHdd(LOG_DEBUG, "NetAdapter::conOpen - %s remote %d.%d.%d.%d:%d, local port %d, buff_size %d",
           tcpNotUdp ? "TCP" : "UDP", (remoteHost >> 24) & 0xff, (remoteHost >> 16) & 0xff, (remoteHost >> 8) & 0xff,
           remoteHost & 0xff, remotePort, localPort, buff_size);

    if(remoteHost != 0) {
        conOpen_connect(slot, tcpNotUdp, localPort, remoteHost, remotePort, buff_size);
    } else {
        conOpen_listen (slot, tcpNotUdp, localPort, buff_size);
    }
}

void NetAdapter::conOpen_listen(int slot, bool tcpNotUdp, uint16_t localPort, uint16_t buff_size)
{
    int fd = Utils::socketCloexec(AF_INET, tcpNotUdp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if(fd < 0) {
        dataTrans->setStatus(E_CONNECTFAIL);
        return;
    }

    setNonBlocking(fd);

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(localPort);       // 0 = any free port

    if(bind(fd, (struct sockaddr *) &local, sizeof(local)) == -1) {
        int err = errno;
        close(fd);
        fd = -1;

        // Real STinG lets more handles listen on the same port. Here they share the listening socket (dup() - the
        // same socket, the first one that accept()s gets the connection).
        if(tcpNotUdp && err == EADDRINUSE && localPort != 0) {
            for(int i=0; i<NET_HANDLES_COUNT; i++) {
                if(!cons[i].isFree() && cons[i].type == TCP && cons[i].listenFd != -1 && cons[i].localPort == localPort) {
                    fd = dup(cons[i].listenFd);
                    break;
                }
            }
        }

        if(fd == -1) {
            logHdd(LOG_DEBUG, "NetAdapter::conOpen_listen - bind() to port %d failed: %s", localPort, strerror(err));
            dataTrans->setStatus(E_CONNECTFAIL);
            return;
        }
    } else if(tcpNotUdp) {
        listen(fd, 4);
    }

    if(localPort == 0) {
        localPort = getLocalPort(fd);
    }

    TNetConnection *nc = &cons[slot];
    nc->initVars();
    nc->inUse       = true;
    nc->localPort   = localPort;
    nc->type        = tcpNotUdp ? TCP : UDP;
    nc->buff_size   = buff_size;

    if(tcpNotUdp) {                                 // TCP: wait for a connection, accept() in updateCons_passive()
        nc->activeNotPassive = false;
        nc->listenFd         = fd;
        nc->status           = TLISTEN;
    } else {                                        // UDP: just receive on the bound socket
        nc->fd               = fd;
        nc->status           = TESTABLISH;
        nc->readWrapper.init(fd, UDP, buff_size);
    }

    logHdd(LOG_DEBUG, "NetAdapter::conOpen_listen - %s on port %d, handle %d", tcpNotUdp ? "listening" : "UDP bound",
           localPort, network_slotToHandle(slot));
    dataTrans->setStatus(network_slotToHandle(slot));
}

void NetAdapter::conOpen_connect(int slot, bool tcpNotUdp, uint16_t localPort, uint32_t remoteHost, uint16_t remotePort, uint16_t buff_size)
{
    int fd = Utils::socketCloexec(AF_INET, tcpNotUdp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if(fd < 0) {
        dataTrans->setStatus(E_CONNECTFAIL);
        return;
    }

    setNonBlocking(fd);

    if(localPort != 0) {                            // the ST wants this local port (TCP_open() / UDP_open() with CAB)
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = htons(localPort);

        if(bind(fd, (struct sockaddr *) &local, sizeof(local)) == -1) {
            logHdd(LOG_DEBUG, "NetAdapter::conOpen_connect - bind() to local port %d failed: %s", localPort, strerror(errno));
            close(fd);
            dataTrans->setStatus(E_CONNECTFAIL);
            return;
        }
    }

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family      = AF_INET;
    remote.sin_addr.s_addr = htonl(remoteHost);
    remote.sin_port        = htons(remotePort);

    TNetConnection *nc = &cons[slot];
    nc->initVars();
    nc->inUse            = true;
    nc->activeNotPassive = true;
    nc->remote_adr       = remote;
    nc->type             = tcpNotUdp ? TCP : UDP;
    nc->buff_size        = buff_size;

    // TCP: start connecting, UDP: set the default destination
    int ires = connect(fd, (struct sockaddr *) &remote, sizeof(remote));

    if(ires < 0 && errno != EINPROGRESS) {          // failed right away - real STinG gives a handle and reports it later
        int err = errno;
        logHdd(LOG_DEBUG, "NetAdapter::conOpen_connect - connect() failed: %s", strerror(err));

        nc->localPort = (localPort != 0) ? localPort : getLocalPort(fd);
        close(fd);

        nc->status   = TCLOSED;
        nc->netError = (err == ECONNREFUSED) ? E_REFUSE : E_UNREACHABLE;
        nc->wasReset = (err == ECONNREFUSED);
        dataTrans->setStatus(network_slotToHandle(slot));
        return;
    }

    if(tcpNotUdp) {
        setKeepAliveOptions(fd);
    }

    nc->fd          = fd;
    nc->localPort   = (localPort != 0) ? localPort : getLocalPort(fd);
    nc->status      = (ires < 0) ? TSYN_SENT : TESTABLISH;    // EINPROGRESS - still connecting
    nc->readWrapper.init(fd, nc->type, buff_size);

    logHdd(LOG_DEBUG, "NetAdapter::conOpen_connect - handle %d, local port %d, status %d", network_slotToHandle(slot),
           nc->localPort, nc->status);
    dataTrans->setStatus(network_slotToHandle(slot));
}

//----------------------------------------------
// data: handle (W), timeout (W, TCP only)
// TCP: we close an open connection -> TTIME_WAIT for a while; after the remote closed: TLAST_ACK for a while with
// timeout 0, gone with a timeout; after a reset: E_RRESET; a 2nd close of a lingering handle: E_BADCLOSE
void NetAdapter::conClose(void)
{
    if(!dataTrans->recvData(dataBuffer, 512)) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    bool tcpNotUdp  = (cmd[4] == NET_CMD_TCP_CLOSE);
    int  timeout    = (int16_t) Utils::getWord(dataBuffer + 2);

    TNetConnection *nc = connFromHandle(Utils::getWord(dataBuffer), "conClose");
    if(!nc) {
        return;
    }

    if(nc->type != (tcpNotUdp ? TCP : UDP)) {       // TCP_close() on UDP handle and the other way round
        dataTrans->setStatus(E_BADHANDLE);
        return;
    }

    if(!tcpNotUdp) {
        nc->closeIt();
        dataTrans->setStatus(E_NORMAL);
        return;
    }

    if(nc->status == TTIME_WAIT || nc->status == TLAST_ACK) {   // already closed by the ST, still lingering
        dataTrans->setStatus(E_BADCLOSE);
        return;
    }

    if(nc->status == TCLOSE_WAIT && nc->fd != -1) {     // sent something after the remote closed? its RST may be on the way
        struct pollfd pfd = { nc->fd, 0, 0 };     // no events: waits only for POLLERR / POLLHUP (reset), up to 20 ms
        poll(&pfd, 1, 20);
    }
    checkTcpSocket(nc);                             // a reset that just came - TCP_close() reports it, as STinG

    int8_t result = nc->wasReset ? E_RRESET : E_NORMAL;

    switch(nc->status) {
        case TESTABLISH:                            // we close first - the remote still has to answer
            flushPending(nc);
            nc->closeSockets();                     // close() sends what's still queued, then FIN
            nc->status      = TTIME_WAIT;
            nc->lingerStart = Utils::getCurrentMs();
            nc->lingerMs    = LINGER_TIME_WAIT_MS;
            break;

        case TCLOSE_WAIT:                           // the remote closed first
            nc->closeSockets();
            if(timeout == 0) {                      // not waiting for the end - the handle stays for a moment
                nc->status      = TLAST_ACK;
                nc->lingerStart = Utils::getCurrentMs();
                nc->lingerMs    = LINGER_LAST_ACK_MS;
            } else {
                nc->closeIt();
            }
            break;

        default:                                    // listening, connecting, reset / refused - just gone
            nc->closeIt();
            break;
    }

    dataTrans->setStatus(result);
}

//----------------------------------------------
void NetAdapter::flushPending(TNetConnection *nc)
{
    if(nc->pending.empty() || nc->fd == -1) {
        return;
    }

    ssize_t res = send(nc->fd, nc->pending.data(), nc->pending.size(), SEND_NOSIGNAL);

    if(res > 0) {
        nc->pending.erase(nc->pending.begin(), nc->pending.begin() + res);
    }
}

void NetAdapter::conSend(void)
{
    int      cmdType = cmd[4];
    int      length  = Utils::getWord(cmd + 6);
    bool     isOdd   = cmd[8];                      // the data were sent from an odd ST address...
    uint8_t  oddByte = cmd[9];                      // ...then this is the 1st byte of them

    // the data come in any case - get them before anything else
    if(length > NET_BUFFER_SIZE - 512) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    int lenRoundUp = (length + 511) & ~511;         // the ST sends whole sectors

    uint8_t *pData = dataBuffer;
    if(isOdd) {
        dataBuffer[0] = oddByte;
        pData = dataBuffer + 1;
    }

    if(lenRoundUp > 0 && !dataTrans->recvData(pData, lenRoundUp)) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    TNetConnection *nc = connFromHandle(cmd[5], "conSend");
    if(!nc) {
        return;
    }

    if((cmdType == NET_CMD_TCP_SEND && nc->type != TCP) || (cmdType == NET_CMD_UDP_SEND && nc->type != UDP)) {
        dataTrans->setStatus(E_BADHANDLE);
        return;
    }

    if(nc->type == UDP) {                           // a datagram goes whole or not at all
        if(nc->fd == -1) {
            dataTrans->setStatus(E_BADHANDLE);
            return;
        }

        ssize_t res = send(nc->fd, dataBuffer, length, SEND_NOSIGNAL);
        if(res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            dataTrans->setStatus(E_OBUFFULL);
            return;
        }
        // other errors (e.g. ECONNREFUSED of the previous datagram) - real STinG doesn't know about them either
        dataTrans->setStatus(E_NORMAL);
        return;
    }

    switch(nc->status) {
        case TLISTEN:
            dataTrans->setStatus(E_LISTEN);
            return;

        case TESTABLISH:
        case TCLOSE_WAIT:                           // the remote closed only its side - sending is fine
            break;

        case TSYN_SENT:                             // not connected yet - keep it, it goes out when connected
            if(sendQueueFull(nc, length)) {
                dataTrans->setStatus(E_OBUFFULL);
                return;
            }
            nc->pending.insert(nc->pending.end(), dataBuffer, dataBuffer + length);
            dataTrans->setStatus(E_NORMAL);
            return;

        default:                                    // closed, reset, lingering after close
            dataTrans->setStatus(E_EOF);
            return;
    }

    if(sendQueueFull(nc, length)) {
        dataTrans->setStatus(E_OBUFFULL);
        return;
    }

    // send all of it, waiting a bit for room in the socket buffer - returning E_OBUFFULL after a part was already
    // sent would make the ST send the whole block again
    int written = 0;
    Timeout timeout(SEND_WAIT_MS);

    while(written < length) {
        ssize_t res = send(nc->fd, dataBuffer + written, length - written, SEND_NOSIGNAL);

        if(res > 0) {
            written += res;
            continue;
        }

        if(res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if(timeout.expired()) {
                break;
            }

            struct pollfd pfd = { nc->fd, POLLOUT, 0 };
            poll(&pfd, 1, 50);
            continue;
        }

        logHdd(LOG_DEBUG, "NetAdapter::conSend - send() failed after %d of %d bytes: %s", written, length, strerror(errno));
        nc->wasReset = true;                        // the remote is gone
        nc->status   = TCLOSED;
        dataTrans->setStatus(E_RRESET);
        return;
    }

    if(written < length) {
        logHdd(LOG_DEBUG, "NetAdapter::conSend - socket full, sent %d of %d bytes", written, length);
        dataTrans->setStatus((written == 0) ? E_OBUFFULL : E_NORMAL);  // part sent: the rest is lost, but don't duplicate
        return;
    }

    dataTrans->setStatus(E_NORMAL);
}

//----------------------------------------------
// reply, N = NET_HANDLES_COUNT: N x DW bytes to read, N x B state, N x W local port, N x DW remote host,
// N x W remote port, N x B error (reported once), N x B flags (bit 0: handle in use), DW ICMP bytes waiting
void NetAdapter::conUpdateInfo(void)
{
    updateCons();

    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataDword(cons[i].bytesInSocket);
    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataByte (cons[i].status);
    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataWord (cons[i].localPort);
    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataDword(ntohl(cons[i].remote_adr.sin_addr.s_addr));
    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataWord (ntohs(cons[i].remote_adr.sin_port));

    for(int i=0; i<NET_HANDLES_COUNT; i++) {
        dataTrans->addDataByte((uint8_t) cons[i].netError);
        cons[i].netError = 0;                       // the ST has it now, it reports it once
    }

    for(int i=0; i<NET_HANDLES_COUNT; i++) dataTrans->addDataByte(cons[i].inUse ? 1 : 0);

    icmpWrapper.receiveAll();
    dataTrans->addDataDword(icmpWrapper.calcDataByteCountTotal());

    dataTrans->padDataToMul16();
    dataTrans->setStatus(E_NORMAL);
}

void NetAdapter::updateCons(void)
{
    uint32_t now = Utils::getCurrentMs();

    for(int i=0; i<NET_HANDLES_COUNT; i++) {
        TNetConnection *nc = &cons[i];

        if(nc->isFree()) {
            continue;
        }

        if(nc->status == TTIME_WAIT || nc->status == TLAST_ACK) {  // closed by the ST, lingering
            if(now - nc->lingerStart >= nc->lingerMs) {
                nc->closeIt();
            }
            continue;
        }

        if(nc->activeNotPassive) {
            updateCons_active(i);
        } else {
            updateCons_passive(i);
        }
    }
}

void NetAdapter::updateCons_passive(int i)
{
    TNetConnection *nc = &cons[i];

    if(nc->fd == -1 && nc->listenFd != -1) {        // listening - did somebody connect?
        struct sockaddr_in remote;
        socklen_t len = sizeof(remote);
        memset(&remote, 0, sizeof(remote));

        int fd = Utils::acceptCloexec(nc->listenFd, (struct sockaddr *) &remote, &len);
        if(fd == -1) {
            return;                                 // nobody yet
        }

        setNonBlocking(fd);
        setKeepAliveOptions(fd);

        close(nc->listenFd);                        // as STinG: the handle becomes the connection, it doesn't listen any more
        nc->listenFd    = -1;
        nc->fd          = fd;
        nc->status      = TESTABLISH;
        nc->remote_adr  = remote;
        nc->readWrapper.init(fd, nc->type, nc->buff_size);

        logHdd(LOG_DEBUG, "NetAdapter::updateCons_passive - connection %d accepted from %s:%d", i,
               inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
        return;
    }

    checkTcpSocket(nc);
}

void NetAdapter::updateCons_active(int i)
{
    TNetConnection *nc = &cons[i];

    if(nc->type != TCP) {                           // UDP has no state
        nc->bytesInSocket = nc->readWrapper.getCount();
        return;
    }

    if(nc->status == TSYN_SENT) {                   // connecting - finished?
        struct pollfd pfd = { nc->fd, POLLOUT, 0 };
        if(poll(&pfd, 1, 0) <= 0) {
            return;                                 // still connecting
        }

        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(nc->fd, SOL_SOCKET, SO_ERROR, &err, &len);

        if(err == 0) {
            nc->status = TESTABLISH;
            flushPending(nc);
            logHdd(LOG_DEBUG, "NetAdapter::updateCons_active - connection %d established", i);
        } else {                                    // refused / unreachable - the handle stays until TCP_close()
            logHdd(LOG_DEBUG, "NetAdapter::updateCons_active - connection %d failed: %s", i, strerror(err));
            nc->closeSockets();
            nc->status   = TCLOSED;
            nc->netError = (err == ECONNREFUSED) ? E_REFUSE : E_UNREACHABLE;
            nc->wasReset = (err == ECONNREFUSED);   // a refused connection is a RST - TCP_close() gives E_RRESET
        }
        return;
    }

    checkTcpSocket(nc);
}

// TCP state from the kernel - also when unread data are waiting, where poll() / MSG_PEEK can't tell (macOS has no
// POLLRDHUP, and no POLLERR after a RST while data wait)
#define KSTATE_UNKNOWN          0
#define KSTATE_OPEN             1
#define KSTATE_REMOTE_CLOSED    2       // FIN from remote (CLOSE_WAIT, LAST_ACK)
#define KSTATE_CLOSED           3       // reset, timed out

static int kernelTcpState(int fd)
{
#if defined(__linux__) && defined(TCP_INFO)
    struct tcp_info ti;
    socklen_t len = sizeof(ti);
    memset(&ti, 0, sizeof(ti));

    if(getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &len) == 0) {
        switch(ti.tcpi_state) {
            case TCP_CLOSE_WAIT:
            case TCP_LAST_ACK:      return KSTATE_REMOTE_CLOSED;
            case TCP_CLOSE:         return KSTATE_CLOSED;
            default:                return KSTATE_OPEN;
        }
    }
#elif defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
    struct tcp_connection_info ci;
    socklen_t len = sizeof(ci);
    memset(&ci, 0, sizeof(ci));

    if(getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &ci, &len) == 0) {
        switch(ci.tcpi_state) {
            case TCPS_CLOSE_WAIT:
            case TCPS_LAST_ACK:     return KSTATE_REMOTE_CLOSED;
            case TCPS_CLOSED:       return KSTATE_CLOSED;
            default:                return KSTATE_OPEN;
        }
    }
#else
    (void) fd;
#endif
    return KSTATE_UNKNOWN;
}

bool NetAdapter::seesRemoteCloseWithData(void)
{
#if defined(POLLRDHUP) || (defined(__linux__) && defined(TCP_INFO)) || (defined(__APPLE__) && defined(TCP_CONNECTION_INFO))
    return true;
#else
    return false;
#endif
}

// established / closing TCP connection: data count, remote closed (TCLOSE_WAIT), reset (TCLOSED + E_RRESET once).
// Data that came before the FIN / RST can still be read - the socket is closed only when they're used up.
void NetAdapter::checkTcpSocket(TNetConnection *nc)
{
    if(nc->fd == -1) {
        nc->bytesInSocket = 0;
        return;
    }

    nc->bytesInSocket = nc->readWrapper.getCount();

    if(nc->status == TESTABLISH || nc->status == TCLOSE_WAIT) {
        int err = 0;                                // a pending error (reset) - poll() doesn't always say POLLERR
        socklen_t errLen = sizeof(err);
        getsockopt(nc->fd, SOL_SOCKET, SO_ERROR, &err, &errLen);

        int kstate = kernelTcpState(nc->fd);

        short events = POLLIN;
#ifdef POLLRDHUP
        events |= POLLRDHUP;                        // Linux: remote closed, even with data still waiting
#endif
        struct pollfd pfd = { nc->fd, events, 0 };
        poll(&pfd, 1, 0);

        if(err != 0 || (pfd.revents & POLLERR) || kstate == KSTATE_CLOSED) {      // RST (or timed out)
            logHdd(LOG_DEBUG, "NetAdapter::checkTcpSocket - handle %d reset: %s", network_slotToHandle(nc - cons),
                   err ? strerror(err) : "closed");
            nc->status   = TCLOSED;
            nc->netError = E_RRESET;
            nc->wasReset = true;
        } else if(nc->status == TESTABLISH) {
            bool remoteClosed = (kstate == KSTATE_REMOTE_CLOSED);
#ifdef POLLRDHUP
            remoteClosed = remoteClosed || (pfd.revents & POLLRDHUP) != 0;
#endif
            if(!remoteClosed && kstate == KSTATE_UNKNOWN && nc->bytesInSocket == 0) {  // elsewhere: EOF when nothing more to read
                uint8_t b;
                ssize_t res = recv(nc->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
                if(res == 0) {
                    remoteClosed = true;
                } else if(res < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    nc->status   = TCLOSED;
                    nc->netError = E_RRESET;
                    nc->wasReset = true;
                }
            }

            if(remoteClosed && nc->status == TESTABLISH) {
                logHdd(LOG_DEBUG, "NetAdapter::checkTcpSocket - handle %d closed by remote", network_slotToHandle(nc - cons));
                nc->status = TCLOSE_WAIT;
            }
        }
    }

    if(nc->status == TCLOSED && nc->bytesInSocket == 0) {  // reset, and the data before it used up - the socket can go
        nc->closeSockets();
    }
}

//----------------------------------------------
void NetAdapter::conGetCharBuffer(void)
{
    TNetConnection *nc = connFromHandle(cmd[5], "conGetCharBuffer");
    if(!nc) {
        return;
    }

    removeUsedChars(nc, cmd[9]);

    int gotBytes = nc->readWrapper.peekBlock(dataBuffer, 250);    // < 255, charsUsed comes back as a byte
    if(gotBytes < 0) {
        gotBytes = 0;
    }

    dataTrans->addDataByte(gotBytes);

    if(gotBytes > 0) {
        dataTrans->addDataBfr(dataBuffer, gotBytes, true);
    } else {
        memset(dataBuffer, 0, 32);
        dataTrans->addDataBfr(dataBuffer, 32, true);
    }

    dataTrans->setStatus(E_NORMAL);
}

// The fake STinG keeps up to CN_PEEK_MAX bytes of the stream locally and finds CNgets() lines there, without a command
// for each line. Like CNGET_CHAR: the bytes stay in the socket until the ST says it used them (next PEEK / other call).
void NetAdapter::conPeek(void)
{
    int used = Utils::getWord(cmd + 6);
    int max  = Utils::getWord(cmd + 8);

    TNetConnection *nc = connFromHandle(cmd[5], "conPeek");
    if(!nc) {
        return;
    }

    removeUsedChars(nc, used);

    if(max > CN_PEEK_MAX) {
        max = CN_PEEK_MAX;
    }

    int gotBytes = (max > 0) ? nc->readWrapper.peekBlock(dataBuffer, max) : 0;
    if(gotBytes < 0) {
        gotBytes = 0;
    }

    dataTrans->addDataWord(gotBytes);
    dataTrans->addDataBfr(dataBuffer, gotBytes, true);
    dataTrans->setStatus(E_NORMAL);
}

void NetAdapter::conGetNdb(void)
{
    TNetConnection *nc = connFromHandle(cmd[5], "conGetNdb");
    if(!nc) {
        return;
    }

    removeUsedChars(nc, cmd[9]);

    if(cmd[6]) {                                    // the data
        int ndbSize = nc->readWrapper.getNdb(dataBuffer);
        dataTrans->addDataBfr(dataBuffer, ndbSize, true);
    } else {                                        // just the size of the next one
        int nextSizeBytes   = nc->readWrapper.getNextNdbSize();
        int nextSizeSectors = (nextSizeBytes + 511) >> 9;

        dataTrans->addDataDword(nextSizeBytes);
        dataTrans->addDataByte(nextSizeSectors);
        dataTrans->padDataToMul16();
    }

    dataTrans->setStatus(E_NORMAL);
}

void NetAdapter::conGetBlock(void)
{
    int wantedLength = Utils::getWord(cmd + 6);

    TNetConnection *nc = connFromHandle(cmd[5], "conGetBlock");
    if(!nc) {
        return;
    }

    removeUsedChars(nc, cmd[9]);

    if(wantedLength > NET_BUFFER_SIZE || nc->readWrapper.getCount() < wantedLength) {  // all or nothing, as STinG
        dataTrans->setStatus(E_NODATA);
        return;
    }

    nc->readWrapper.peekBlock(dataBuffer, wantedLength);
    nc->readWrapper.removeBlock(wantedLength);

    dataTrans->addDataBfr(dataBuffer, wantedLength, true);
    dataTrans->setStatus(E_NORMAL);
}

// reply: length (W, without the terminator), then the line with the delimiter replaced by 0 - the line can contain
// 0 bytes, so the ST must not count it with strlen()
void NetAdapter::conGetString(void)
{
    int     maxLength = Utils::getWord(cmd + 6);
    uint8_t delim     = cmd[8];

    TNetConnection *nc = connFromHandle(cmd[5], "conGetString");
    if(!nc) {
        return;
    }

    removeUsedChars(nc, cmd[9]);

    int streamLength = nc->readWrapper.peekBlock(dataBuffer, NET_BUFFER_SIZE - 1);

    int foundIndex = -1;
    for(int i=0; i<streamLength; i++) {
        if(dataBuffer[i] == delim) {
            foundIndex = i;
            break;
        }
    }

    if(foundIndex == -1) {                          // no complete line yet
        dataTrans->setStatus(E_NODATA);
        return;
    }

    if(foundIndex >= maxLength) {                   // line + terminator doesn't fit
        dataTrans->setStatus(E_BIGBUF);
        return;
    }

    dataBuffer[foundIndex] = 0;                     // the delimiter becomes the terminator
    dataTrans->addDataWord(foundIndex);
    dataTrans->addDataBfr(dataBuffer, foundIndex + 1, true);
    nc->readWrapper.removeBlock(foundIndex + 1);    // the line and the delimiter

    dataTrans->setStatus(E_NORMAL);
}

//----------------------------------------------
void NetAdapter::icmpSend(void)
{
    bool     evenNotOdd = (cmd[4] == NET_CMD_ICMP_SEND_EVEN);
    uint32_t destinIP   = Utils::getDword(cmd + 5);
    int      icmpType   = cmd[9] >> 3;
    int      icmpCode   = cmd[9] & 0x07;
    uint16_t length     = Utils::getWord(cmd + 10);

    int lenRoundUp = ((length + (evenNotOdd ? 0 : 1)) + 511) & ~511;

    if(lenRoundUp > NET_BUFFER_SIZE || (lenRoundUp > 0 && !dataTrans->recvData(dataBuffer, lenRoundUp))) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    uint8_t *pData = evenNotOdd ? dataBuffer : dataBuffer + 1;     // odd: the ST sent from 1 byte before the data
    dataTrans->setStatus(icmpWrapper.send(destinIP, icmpType, icmpCode, length, pData));
}

void NetAdapter::icmpGetDgrams(void)
{
    icmpWrapper.receiveAll();

    if(icmpWrapper.calcDataByteCountTotal() == 0) {
        dataTrans->setStatus(E_NODATA);
        return;
    }

    int bufferSizeBytes = cmd[5] * 512;
    int fit = icmpWrapper.calcHowManyDatagramsFitIntoBuffer(bufferSizeBytes);

    for(int i=0; i<fit; i++) {
        int index = icmpWrapper.getNonEmptyIndex();
        if(index == -1) {
            break;
        }

        TStingDgram *d = &icmpWrapper.dgrams[index];
        dataTrans->addDataWord(d->count);
        dataTrans->addDataBfr(d->data, d->count, false);
        d->clear();
    }

    dataTrans->addDataWord(0);                      // no more datagrams
    dataTrans->padDataToMul16();
    dataTrans->setStatus(E_NORMAL);
}

//----------------------------------------------
void NetAdapter::resolveStart(void)
{
    if(!dataTrans->recvData(dataBuffer, 512)) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    dataBuffer[511] = 0;
    const char *name = (const char *) dataBuffer;

    bool gotNormalChar = false;
    for(int i=0; name[i]; i++) {
        if(isalnum((unsigned char) name[i])) {
            gotNormalChar = true;
            break;
        }
    }

    if(!gotNormalChar) {                            // empty, spaces, ...
        dataTrans->setStatus(E_CANTRESOLVE);
        return;
    }

    int slot = resolver.addRequest(name);
    dataTrans->setStatus((slot < 0) ? (uint8_t) E_CANTRESOLVE : (uint8_t) slot);
}

// reply: real name (256 B), count (B), 0 (B), IPs (DW each); status E_NORMAL also when nothing was found (count 0)
void NetAdapter::resolveGetResp(void)
{
    int index = cmd[5];

    if(!resolver.slotIndexValid(index)) {
        dataTrans->setStatus(E_PARAMETER);
        return;
    }

    if(!resolver.checkAndhandleSlot(index)) {
        dataTrans->setStatus(RES_DIDNT_FINISH_YET);
        return;
    }

    std::shared_ptr<ResolveJob> job = resolver.getJob(index);

    uint8_t name[256];
    memset(name, 0, sizeof(name));
    strncpy((char *) name, job->canonName.c_str(), 255);

    dataTrans->addDataBfr(name, 256, false);        // real name, 256 B
    dataTrans->addDataByte(job->count);             // [256] IP count
    dataTrans->addDataByte(0);                      // [257]

    for(int i=0; i<job->count; i++) {               // [258 ..] IPs
        dataTrans->addDataDword(job->ips[i]);
    }

    dataTrans->padDataToMul16();

    logHdd(LOG_DEBUG, "NetAdapter::resolveGetResp - slot %d: '%s' -> %d IPs, error %d", index, job->hostName.c_str(),
           job->count, job->error);

    resolver.clearSlot(index);
    dataTrans->setStatus(E_NORMAL);
}

//----------------------------------------------
const char *NetAdapter::cmdName(uint8_t c)
{
    switch(c) {
        case NET_CMD_IDENTIFY:              return "IDENTIFY";
        case NET_CMD_TCP_OPEN:              return "TCP_OPEN";
        case NET_CMD_TCP_CLOSE:             return "TCP_CLOSE";
        case NET_CMD_TCP_SEND:              return "TCP_SEND";
        case NET_CMD_TCP_WAIT_STATE:        return "TCP_WAIT_STATE";
        case NET_CMD_TCP_ACK_WAIT:          return "TCP_ACK_WAIT";
        case NET_CMD_TCP_INFO:              return "TCP_INFO";
        case NET_CMD_UDP_OPEN:              return "UDP_OPEN";
        case NET_CMD_UDP_CLOSE:             return "UDP_CLOSE";
        case NET_CMD_UDP_SEND:              return "UDP_SEND";
        case NET_CMD_ICMP_SEND_EVEN:        return "ICMP_SEND_EVEN";
        case NET_CMD_ICMP_SEND_ODD:         return "ICMP_SEND_ODD";
        case NET_CMD_ICMP_HANDLER:          return "ICMP_HANDLER";
        case NET_CMD_ICMP_DISCARD:          return "ICMP_DISCARD";
        case NET_CMD_ICMP_GET_DGRAMS:       return "ICMP_GET_DGRAMS";
        case NET_CMD_CNKICK:                return "CNKICK";
        case NET_CMD_CNBYTE_COUNT:          return "CNBYTE_COUNT";
        case NET_CMD_CNGET_CHAR:            return "CNGET_CHAR";
        case NET_CMD_CNGET_NDB:             return "CNGET_NDB";
        case NET_CMD_CNGET_BLOCK:           return "CNGET_BLOCK";
        case NET_CMD_CNGETINFO:             return "CNGETINFO";
        case NET_CMD_CNGETS:                return "CNGETS";
        case NET_CMD_CN_UPDATE_INFO:        return "CN_UPDATE_INFO";
        case NET_CMD_CN_PEEK:               return "CN_PEEK";
        case NET_CMD_RESOLVE:               return "RESOLVE";
        case NET_CMD_ON_PORT:               return "ON_PORT";
        case NET_CMD_OFF_PORT:              return "OFF_PORT";
        case NET_CMD_QUERY_PORT:            return "QUERY_PORT";
        case NET_CMD_CNTRL_PORT:            return "CNTRL_PORT";
        case NET_CMD_RESOLVE_GET_RESPONSE:  return "RESOLVE_GET_RESPONSE";
        default:                            return "unknown";
    }
}
