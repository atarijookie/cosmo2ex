// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of the network module (HOSTMOD_NETWORK_ADAPTER) - the host side of the fake STinG. The commands are what
// ce_fakesting sends, the other side of the connections is a socket in the test on 127.0.0.1.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/utils.h"
#include "hdd/acsidatatrans.h"
#include "hdd/network/netadapter.h"
#include "hdd/network/netadapter_commands.h"
#include "hdd/network/sting.h"

namespace {

#define LOOPBACK    0x7f000001

// instead of the device: gives the module the data the ST would send, keeps the reply and the status
class NetFakeDataTrans : public AcsiDataTrans
{
public:
    std::vector<uint8_t> fromSt;
    std::vector<uint8_t> reply;
    uint8_t lastStatus = 0xff;

    bool recvData(uint8_t *data, uint32_t cnt) {
        memset(data, 0, cnt);
        memcpy(data, fromSt.data(), fromSt.size() < cnt ? fromSt.size() : cnt);
        return true;
    }

    void setStatus(uint8_t stat) {
        lastStatus = stat;
        AcsiDataTrans::setStatus(stat);
    }

    void sendDataAndStatus(bool fromRetryModule = false) {
        (void) fromRetryModule;
        reply.assign(buffer, buffer + count);
    }
};

struct NetHarness {
    NetFakeDataTrans dt;
    NetAdapter      *na;

    NetHarness()  { na = new NetAdapter(&dt); }
    ~NetHarness() { delete na; }

    // pCmd as the module gets it: [1..2] 'CE', [3] module, [4] command, [5..] arguments
    uint8_t command(uint8_t netCmd, std::vector<uint8_t> args = {}, std::vector<uint8_t> data = {}) {
        uint8_t cmd[16];
        memset(cmd, 0, sizeof(cmd));
        cmd[1] = 'C';
        cmd[2] = 'E';
        cmd[3] = HOSTMOD_NETWORK_ADAPTER;
        cmd[4] = netCmd;
        for(size_t i=0; i<args.size() && i<11; i++) {
            cmd[5 + i] = args[i];
        }

        dt.fromSt = data;
        dt.reply.clear();
        dt.lastStatus = 0xff;
        na->processCommand(cmd);
        return dt.lastStatus;
    }

    // TCP_OPEN / UDP_OPEN, returns the status (handle or error)
    uint8_t open(bool tcp, uint32_t host, uint16_t port, uint16_t buffSize = 2000, uint16_t localPort = 0) {
        std::vector<uint8_t> d(16, 0);
        Utils::storeDword(d.data(),      host);
        Utils::storeWord (d.data() + 4,  port);
        Utils::storeWord (d.data() + 8,  buffSize);
        Utils::storeWord (d.data() + 14, localPort);
        return command(tcp ? NET_CMD_TCP_OPEN : NET_CMD_UDP_OPEN, {}, d);
    }

    uint8_t closeIt(bool tcp, uint8_t handle, uint16_t timeout = 0) {
        std::vector<uint8_t> d(4, 0);
        Utils::storeWord(d.data(), handle);
        Utils::storeWord(d.data() + 2, timeout);
        return command(tcp ? NET_CMD_TCP_CLOSE : NET_CMD_UDP_CLOSE, {}, d);
    }

    uint8_t send(bool tcp, uint8_t handle, const std::string& s) {
        uint16_t len = s.size();
        return command(tcp ? NET_CMD_TCP_SEND : NET_CMD_UDP_SEND, {handle, (uint8_t) (len >> 8), (uint8_t) len, 0, 0},
                       std::vector<uint8_t>(s.begin(), s.end()));
    }

    struct Info { int bytes; int status; int lport; uint32_t rhost; int rport; int error; int inUse; };

    // CN_UPDATE_INFO, N handles: N x DW bytes, N x B state, N x W lport, N x DW rhost, N x W rport, N x B error,
    // N x B in use, DW ICMP bytes
    Info info(uint8_t handle) {
        const int N = NET_HANDLES_COUNT;
        command(NET_CMD_CN_UPDATE_INFO);
        REQUIRE(dt.reply.size() >= (size_t) (15 * N + 4));
        int i = network_handleToSlot(handle);
        uint8_t *r = dt.reply.data();
        return { (int) Utils::getDword(r + i * 4), r[4 * N + i], Utils::getWord(r + 5 * N + i * 2),
                 Utils::getDword(r + 7 * N + i * 4), Utils::getWord(r + 11 * N + i * 2), (int8_t) r[13 * N + i],
                 r[14 * N + i] };
    }

    uint32_t icmpBytes(void) {
        command(NET_CMD_CN_UPDATE_INFO);
        return Utils::getDword(dt.reply.data() + 15 * NET_HANDLES_COUNT);
    }

    // poll CN_UPDATE_INFO until the condition holds, up to 2 s
    template<typename F> Info waitFor(uint8_t handle, F cond) {
        Info inf = info(handle);
        for(int i=0; i<200 && !cond(inf); i++) {
            usleep(10000);
            inf = info(handle);
        }
        return inf;
    }

    uint8_t getBlock(uint8_t handle, uint16_t len) {
        return command(NET_CMD_CNGET_BLOCK, {handle, (uint8_t) (len >> 8), (uint8_t) len, 0, 0});
    }

    uint8_t getString(uint8_t handle, uint16_t maxLen, char delim) {
        return command(NET_CMD_CNGETS, {handle, (uint8_t) (maxLen >> 8), (uint8_t) maxLen, (uint8_t) delim, 0});
    }

    std::string replyStr(size_t len) {
        REQUIRE(dt.reply.size() >= len);
        return std::string((const char *) dt.reply.data(), len);
    }
};

int tcpListener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(LOOPBACK);
    REQUIRE(bind(fd, (struct sockaddr *) &a, sizeof(a)) == 0);
    REQUIRE(listen(fd, 4) == 0);
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *) &a, &len);
    *port = ntohs(a.sin_port);
    return fd;
}

int acceptWithTimeout(int listenFd, struct sockaddr_in *peer = NULL)
{
    struct pollfd pfd = { listenFd, POLLIN, 0 };
    REQUIRE(poll(&pfd, 1, 2000) == 1);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int fd = accept(listenFd, (struct sockaddr *) &a, &len);
    REQUIRE(fd >= 0);
    if(peer) *peer = a;
    return fd;
}

std::string recvAll(int fd, size_t want)
{
    std::string s;
    char buf[4096];
    while(s.size() < want) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if(poll(&pfd, 1, 2000) != 1) break;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if(n <= 0) break;
        s.append(buf, n);
    }
    return s;
}

bool isHandle(uint8_t st) { return network_handleIsValid(st); }

} // namespace

TEST_CASE("netadapter: IDENTIFY - name, protocol version, local IP")
{
    NetHarness h;
    REQUIRE(h.command(NET_CMD_IDENTIFY) == E_NORMAL);
    REQUIRE(h.dt.reply.size() >= 40);
    CHECK(Utils::getWord(h.dt.reply.data() + 38) == NET_HANDLES_COUNT);
    CHECK(memcmp(h.dt.reply.data(), "CosmosEx network module", 24) == 0);
    CHECK(Utils::getWord(h.dt.reply.data() + 32) == REQUIRED_NETADAPTER_VERSION);
    CHECK(Utils::getDword(h.dt.reply.data() + 34) == NetAdapter::findLocalIp());
}

TEST_CASE("netadapter: TCP connect, send, receive, CNget_block / CNgets, remote close")
{
    NetHarness h;
    uint16_t port;
    int lfd = tcpListener(&port);

    uint8_t handle = h.open(true, LOOPBACK, port);
    REQUIRE(isHandle(handle));

    struct sockaddr_in peer;
    int sfd = acceptWithTimeout(lfd, &peer);

    NetHarness::Info inf = h.waitFor(handle, [](NetHarness::Info i) { return i.status == TESTABLISH; });
    CHECK(inf.status == TESTABLISH);
    CHECK(inf.rhost == LOOPBACK);
    CHECK(inf.rport == port);
    CHECK(inf.lport == ntohs(peer.sin_port));

    // ST -> remote
    CHECK(h.send(true, handle, "hello world") == E_NORMAL);
    CHECK(recvAll(sfd, 11) == "hello world");

    // remote -> ST
    const char *msg = "line one\nline two\npartial";
    REQUIRE(::send(sfd, msg, strlen(msg), 0) == (ssize_t) strlen(msg));
    inf = h.waitFor(handle, [&](NetHarness::Info i) { return i.bytes == (int) strlen(msg); });
    CHECK(inf.bytes == (int) strlen(msg));

    CHECK(h.getBlock(handle, 100) == (uint8_t) E_NODATA);     // more than available - all or nothing
    CHECK(h.getString(handle, 5, '\n') == (uint8_t) E_BIGBUF);   // "line one" doesn't fit 5
    CHECK(h.getString(handle, 100, '\n') == E_NORMAL);
    CHECK(Utils::getWord(h.dt.reply.data()) == 8);         // length first, the line can contain 0 bytes
    CHECK(std::string((const char *) h.dt.reply.data() + 2, 9) == std::string("line one\0", 9));
    CHECK(h.getBlock(handle, 9) == E_NORMAL);
    CHECK(h.replyStr(9) == "line two\n");
    CHECK(h.getString(handle, 100, '\n') == (uint8_t) E_NODATA); // "partial" has no delimiter
    CHECK(h.info(handle).bytes == 7);

    // remote closes: TCLOSE_WAIT with the data still readable, sending still works (as STinG)
    shutdown(sfd, SHUT_WR);
    inf = h.waitFor(handle, [](NetHarness::Info i) { return i.status == TCLOSE_WAIT; });
    CHECK(inf.status == TCLOSE_WAIT);
    CHECK(inf.bytes == 7);
    CHECK(h.getBlock(handle, 7) == E_NORMAL);
    CHECK(h.replyStr(7) == "partial");
    CHECK(h.send(true, handle, "after") == E_NORMAL);
    CHECK(recvAll(sfd, 5) == "after");

    CHECK(h.closeIt(true, handle, 5) == E_NORMAL);         // with timeout: gone right away
    CHECK(h.info(handle).inUse == 0);
    close(sfd);
    close(lfd);
}

TEST_CASE("netadapter: TCP connect refused - E_REFUSE once, E_EOF on send, E_RRESET on close")
{
    NetHarness h;
    uint16_t port;
    int lfd = tcpListener(&port);
    close(lfd);                                             // nobody listens there now

    uint8_t handle = h.open(true, LOOPBACK, port);
    REQUIRE(isHandle(handle));                              // as STinG: a handle, the error comes later

    NetHarness::Info inf = h.waitFor(handle, [](NetHarness::Info i) { return i.error != 0; });
    CHECK(inf.error == E_REFUSE);
    CHECK(inf.status == TCLOSED);
    CHECK(h.info(handle).error == 0);                      // reported once
    CHECK(h.info(handle).inUse == 1);
    CHECK(h.send(true, handle, "x") == (uint8_t) E_EOF);
    CHECK(h.closeIt(true, handle) == (uint8_t) E_RRESET);
    CHECK(h.info(handle).inUse == 0);
    CHECK(h.closeIt(true, handle) == (uint8_t) E_BADHANDLE);
}

TEST_CASE("netadapter: TCP reset by remote - E_RRESET once, data before it still readable")
{
    NetHarness h;
    uint16_t port;
    int lfd = tcpListener(&port);

    uint8_t handle = h.open(true, LOOPBACK, port);
    REQUIRE(isHandle(handle));
    int sfd = acceptWithTimeout(lfd);
    h.waitFor(handle, [](NetHarness::Info i) { return i.status == TESTABLISH; });

    REQUIRE(::send(sfd, "data", 4, 0) == 4);
    h.waitFor(handle, [](NetHarness::Info i) { return i.bytes == 4; });

    struct linger lg = { 1, 0 };                            // close() sends RST
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(sfd);

    NetHarness::Info inf = h.waitFor(handle, [](NetHarness::Info i) { return i.error != 0; });
    CHECK(inf.error == E_RRESET);
    CHECK(inf.status == TCLOSED);
    if(inf.bytes == 4) {                                    // Linux keeps the data that came before the RST
        CHECK(h.getBlock(handle, 4) == E_NORMAL);
        CHECK(h.replyStr(4) == "data");
    }
    CHECK(h.send(true, handle, "x") == (uint8_t) E_EOF);
    CHECK(h.closeIt(true, handle) == (uint8_t) E_RRESET);
    close(lfd);
}

TEST_CASE("netadapter: TCP close of an open connection - TTIME_WAIT, then gone")
{
    NetHarness h;
    uint16_t port;
    int lfd = tcpListener(&port);

    uint8_t handle = h.open(true, LOOPBACK, port);
    REQUIRE(isHandle(handle));
    int sfd = acceptWithTimeout(lfd);
    h.waitFor(handle, [](NetHarness::Info i) { return i.status == TESTABLISH; });

    CHECK(h.send(true, handle, "bye") == E_NORMAL);
    CHECK(h.closeIt(true, handle) == E_NORMAL);
    CHECK(recvAll(sfd, 3) == "bye");                        // what was queued still goes out

    NetHarness::Info inf = h.info(handle);
    CHECK(inf.status == TTIME_WAIT);
    CHECK(inf.inUse == 1);
    CHECK(h.send(true, handle, "x") == (uint8_t) E_EOF);
    CHECK(h.closeIt(true, handle) == (uint8_t) E_BADCLOSE);

    inf = h.waitFor(handle, [](NetHarness::Info i) { return i.inUse == 0; });   // lingers LINGER_TIME_WAIT_MS
    usleep((LINGER_TIME_WAIT_MS + 200) * 1000);
    CHECK(h.info(handle).inUse == 0);

    close(sfd);
    close(lfd);
}

TEST_CASE("netadapter: TCP send queue limited to 2x buffer size, send while connecting")
{
    NetHarness h;
    uint16_t port;
    int lfd = tcpListener(&port);
    int rcv = 2048;
    setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

    uint8_t handle = h.open(true, LOOPBACK, port, 1000);
    REQUIRE(isHandle(handle));
    CHECK(h.send(true, handle, std::string(500, 'a')) == E_NORMAL);     // maybe still connecting - queued
    int sfd = acceptWithTimeout(lfd);                       // the remote doesn't read

    int sent = 500, full = 0;
    for(int i=0; i<2000 && !full; i++) {
        uint8_t st = h.send(true, handle, std::string(1000, 'b'));
        if(st == E_NORMAL)                      sent += 1000;
        else if(st == (uint8_t) E_OBUFFULL)     full = 1;
        else                                    break;
        h.info(handle);                                     // connection updates (pending data go out)
    }
    CHECK(full == 1);
    CHECK(sent < 200000);                                   // not the megabytes of the host socket buffers
    MESSAGE("sent before E_OBUFFULL: " << sent);

    std::string got = recvAll(sfd, sent);                   // the remote reads - nothing lost, nothing twice
    CHECK((int) got.size() == sent);
    CHECK(got.substr(0, 500) == std::string(500, 'a'));

    close(sfd);
    close(lfd);
}

TEST_CASE("netadapter: TCP with given local port binds to it")
{
    NetHarness h;
    uint16_t port, localPort;
    int lfd = tcpListener(&port);

    int tmp = tcpListener(&localPort);                      // find a free port
    close(tmp);

    uint8_t handle = h.open(true, LOOPBACK, port, 2000, localPort);
    REQUIRE(isHandle(handle));

    struct sockaddr_in peer;
    int sfd = acceptWithTimeout(lfd, &peer);
    CHECK(ntohs(peer.sin_port) == localPort);
    CHECK(h.info(handle).lport == localPort);

    close(sfd);
    close(lfd);
}

TEST_CASE("netadapter: TCP passive - listen, remote connects")
{
    NetHarness h;
    uint8_t handle = h.open(true, 0, 0);
    REQUIRE(isHandle(handle));

    NetHarness::Info inf = h.info(handle);
    CHECK(inf.status == TLISTEN);
    REQUIRE(inf.lport != 0);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(LOOPBACK);
    a.sin_port = htons(inf.lport);
    REQUIRE(connect(cfd, (struct sockaddr *) &a, sizeof(a)) == 0);

    inf = h.waitFor(handle, [](NetHarness::Info i) { return i.status == TESTABLISH; });
    CHECK(inf.status == TESTABLISH);
    CHECK(inf.rhost == LOOPBACK);

    REQUIRE(::send(cfd, "abc", 3, 0) == 3);
    inf = h.waitFor(handle, [](NetHarness::Info i) { return i.bytes == 3; });
    CHECK(h.getBlock(handle, 3) == E_NORMAL);
    CHECK(h.replyStr(3) == "abc");

    close(cfd);
}

TEST_CASE("netadapter: TCP passive twice on the same port, round robin handles")
{
    NetHarness h;
    uint16_t port;
    int tmp = tcpListener(&port);
    close(tmp);

    uint8_t a = h.open(true, 0, 0, 2000, port);
    uint8_t b = h.open(true, 0, 0, 2000, port);
    REQUIRE(isHandle(a));
    REQUIRE(isHandle(b));                                   // as STinG: both listen
    CHECK(a != b);

    CHECK(h.closeIt(true, a) == E_NORMAL);
    uint8_t c = h.open(false, LOOPBACK, 9);
    CHECK(isHandle(c));
    CHECK(c != a);                                          // the closed handle isn't given again right away
}

TEST_CASE("netadapter: UDP - datagrams, NDB per datagram, CNget_block across them")
{
    NetHarness h;

    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(LOOPBACK);
    REQUIRE(bind(ufd, (struct sockaddr *) &a, sizeof(a)) == 0);
    socklen_t len = sizeof(a);
    getsockname(ufd, (struct sockaddr *) &a, &len);
    uint16_t port = ntohs(a.sin_port);

    uint8_t handle = h.open(false, LOOPBACK, port);
    REQUIRE(isHandle(handle));
    CHECK(h.info(handle).status == TESTABLISH);

    CHECK(h.send(false, handle, "ping") == E_NORMAL);
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    char buf[64];
    struct pollfd pfd = { ufd, POLLIN, 0 };
    REQUIRE(poll(&pfd, 1, 2000) == 1);
    REQUIRE(recvfrom(ufd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fromLen) == 4);
    CHECK(memcmp(buf, "ping", 4) == 0);

    sendto(ufd, "AAAA", 4, 0, (struct sockaddr *) &from, fromLen);    // 3 datagrams back
    sendto(ufd, "BBBBBB", 6, 0, (struct sockaddr *) &from, fromLen);
    sendto(ufd, "CC", 2, 0, (struct sockaddr *) &from, fromLen);

    NetHarness::Info inf = h.waitFor(handle, [](NetHarness::Info i) { return i.bytes == 12; });
    CHECK(inf.bytes == 12);

    CHECK(h.command(NET_CMD_CNGET_NDB, {handle, 0, 0, 0, 0}) == E_NORMAL);  // size of the next NDB = 1st datagram
    CHECK(Utils::getDword(h.dt.reply.data()) == 4);
    CHECK(h.command(NET_CMD_CNGET_NDB, {handle, 1, 0, 0, 0}) == E_NORMAL);
    CHECK(h.replyStr(4) == "AAAA");

    CHECK(h.getBlock(handle, 7) == E_NORMAL);              // across the datagrams
    CHECK(h.replyStr(7) == "BBBBBBC");
    CHECK(h.info(handle).bytes == 1);

    CHECK(h.closeIt(false, handle) == E_NORMAL);
    close(ufd);
}

TEST_CASE("netadapter: bad handles, wrong connection type")
{
    NetHarness h;
    CHECK(h.getBlock(10, 1) == (uint8_t) E_BADHANDLE);     // out of the handle range
    CHECK(h.send(true, 0x50, "x") == (uint8_t) E_BADHANDLE);   // in range, not open

    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t handle = h.open(false, LOOPBACK, 9);
    REQUIRE(isHandle(handle));
    CHECK(h.send(true, handle, "x") == (uint8_t) E_BADHANDLE);  // TCP send on UDP
    CHECK(h.closeIt(true, handle) == (uint8_t) E_BADHANDLE);   // TCP close on UDP
    CHECK(h.closeIt(false, handle) == E_NORMAL);
    close(ufd);
}

TEST_CASE("netadapter: resolve - dotted IP, empty name, localhost")
{
    NetHarness h;

    std::string ip = "127.0.0.1";
    uint8_t slot = h.command(NET_CMD_RESOLVE, {}, std::vector<uint8_t>(ip.begin(), ip.end() + 1));
    REQUIRE(slot < 10);
    REQUIRE(h.command(NET_CMD_RESOLVE_GET_RESPONSE, {slot}) == E_NORMAL);
    CHECK(h.dt.reply[256] == 1);
    CHECK(Utils::getDword(h.dt.reply.data() + 258) == LOOPBACK);

    std::string bad = "1.2.3.400";                          // as STinG 1.26: 400 & 0xff = 144
    slot = h.command(NET_CMD_RESOLVE, {}, std::vector<uint8_t>(bad.begin(), bad.end() + 1));
    REQUIRE(h.command(NET_CMD_RESOLVE_GET_RESPONSE, {slot}) == E_NORMAL);
    CHECK(Utils::getDword(h.dt.reply.data() + 258) == 0x01020390);
    CHECK(std::string((const char *) h.dt.reply.data()) == "1.2.3.144");

    std::string spaces = "   ";
    CHECK(h.command(NET_CMD_RESOLVE, {}, std::vector<uint8_t>(spaces.begin(), spaces.end() + 1)) == (uint8_t) E_CANTRESOLVE);

    std::string lh = "localhost";
    slot = h.command(NET_CMD_RESOLVE, {}, std::vector<uint8_t>(lh.begin(), lh.end() + 1));
    REQUIRE(slot < 10);

    uint8_t st = RES_DIDNT_FINISH_YET;
    for(int i=0; i<500 && st == RES_DIDNT_FINISH_YET; i++) {
        st = h.command(NET_CMD_RESOLVE_GET_RESPONSE, {slot});
        if(st == RES_DIDNT_FINISH_YET) usleep(10000);
    }
    REQUIRE(st == E_NORMAL);
    CHECK(h.dt.reply[256] >= 1);
}

TEST_CASE("netadapter: ICMP echo to 127.0.0.1 comes back as IP_DGRAM with the ST's echo ID")
{
    NetHarness h;

    // ICMP_send data: echo id, sequence, payload
    std::vector<uint8_t> d = { 0xaf, 0xfe, 0x00, 0x07, 'p', 'i', 'n', 'g' };
    uint8_t st = h.command(NET_CMD_ICMP_SEND_EVEN, {0x7f, 0, 0, 1, (uint8_t) (8 << 3), 0, (uint8_t) d.size()}, d);

    if(st == (uint8_t) E_FNAVAIL) {                         // no ping sockets for this user (ping_group_range)
        MESSAGE("ping socket not allowed here - skipping the ICMP test");
        return;
    }
    REQUIRE(st == E_NORMAL);

    uint32_t icmpBytes = 0;
    for(int i=0; i<200 && icmpBytes == 0; i++) {
        icmpBytes = h.icmpBytes();
        if(icmpBytes == 0) usleep(10000);
    }
    REQUIRE(icmpBytes > 0);

    REQUIRE(h.command(NET_CMD_ICMP_GET_DGRAMS, {2}) == E_NORMAL);
    uint8_t *r = h.dt.reply.data();
    uint16_t dlen = Utils::getWord(r);
    REQUIRE(dlen >= 48 + 12);
    uint8_t *dg  = r + 2;
    uint8_t *icmp = dg + 48;
    CHECK(Utils::getDword(dg + 12) == LOOPBACK);           // source IP
    CHECK(Utils::getWord(dg + 30) == 12);                  // pkt_length: 8 B header + 4 B payload
    CHECK(icmp[0] == 0);                                    // echo reply
    CHECK(Utils::getWord(icmp + 4) == 0xaffe);             // the ST's echo ID, not the kernel's
    CHECK(Utils::getWord(icmp + 6) == 7);
    CHECK(memcmp(icmp + 8, "ping", 4) == 0);
}
