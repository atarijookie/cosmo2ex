// Test server for test_ce/test_sting - the STinG compatibility test on the Atari.
// Runs on a Linux PC on the same LAN as the Atari. Every TCP port accepts many connections at once.
//
// TCP:
//   +0  echo
//   +1  blocks: client sends count (16 bit), length (32 bit), pause ms (16 bit); server sends the blocks, then closes
//   +2  lines: client sends count (16 bit); server sends that many "<4 digit length><text>\n" lines, then closes
//   +3  close: client sends ms (16 bit); server closes after that time
//   +8  scenarios (small receive window): client sends cmd, 0, arg1 (16 bit), arg2 (32 bit), see SCEN_* in stest.h
//   +9  nothing listens - connection refused
//   +10 report: sends "<cmd> <count>\n" of the last finished scenario ("- 0\n" while one runs), then closes
//   +11 sink: counts received bytes until EOF, report "C <count>"
//   +20 connect back: client sends IP (32 bit) + port (16 bit), server connects there and echoes
// UDP:
//   +4  echo, +5 blocks, +6 lines (all in one datagram), +7 close (no meaning for UDP, just reads the time)
//
// Usage: sting_server [-p base_port] [-q]

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int portBase = 10000;
static int quiet    = 0;

enum { K_ECHO, K_BLOCKS, K_LINES, K_CLOSE, K_SCEN, K_REPORT, K_SINK, K_CONNBK };
static const char *kindName[] = { "echo", "blocks", "lines", "close", "scen", "report", "sink", "connbk" };

typedef struct Conn {
    int     fd;
    int     kind;
    char    peer[48];

    uint8_t hdr[8];             // header the client sends first (config / scenario)
    int     hdrLen;
    int     hdrNeed;
    int     configured;

    uint8_t *out;               // pending output
    size_t  outLen, outOff, outCap;
    int     closeAfterFlush;    // 1 = close, 2 = reset
    int     shutWr;             // shutdown(SHUT_WR) done

    int64_t eventAt;            // time of the next timed action, 0 = none
    int     counting;           // count received bytes until EOF, then report
    long    count;
    char    scenCmd;
    int64_t stallUntil;         // don't read before this

    int     blocksLeft;         // K_BLOCKS
    uint32_t blockLen;
    int     blockPause;

    struct Conn *next;
} Conn;

static Conn *conns;
static char  report[64] = "- 0\n";

//---------------------------------------------
static int64_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logmsg(const char *fmt, ...)
{
    if(quiet) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void setNonBlock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int makeListener(int port, int tcp, int rcvBuf)
{
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if(fd < 0) { perror("socket"); exit(1); }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(rcvBuf > 0) {            // set before listen(), so accepted sockets get the small window from the start
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(port);

    if(bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        fprintf(stderr, "bind to port %d: %s\n", port, strerror(errno));
        exit(1);
    }
    if(tcp) listen(fd, 16);

    setNonBlock(fd);
    return fd;
}

//---------------------------------------------
static void outAppend(Conn *c, const void *data, size_t len)
{
    if(c->outLen + len > c->outCap) {
        size_t cap = c->outCap ? c->outCap : 4096;
        while(cap < c->outLen + len) cap *= 2;
        c->out = realloc(c->out, cap);
        c->outCap = cap;
    }
    memcpy(c->out + c->outLen, data, len);
    c->outLen += len;
}

static void appendPattern(Conn *c, size_t len)     // same as wBuf on the Atari: byte i = i & 0xff
{
    uint8_t buf[4096];
    size_t done = 0;
    while(done < len) {
        size_t n = len - done > sizeof(buf) ? sizeof(buf) : len - done;
        size_t i;
        for(i=0; i<n; i++) buf[i] = (uint8_t) (done + i);
        outAppend(c, buf, n);
        done += n;
    }
}

static void appendLines(Conn *c, int count)
{
    static const char *lines[10] = {
        "Bacon ipsum dolor amet strip steak turducken meatball short loin rump ham ribeye ham hock turkey.\n",
        "Fatback shank turducken, drumstick chuck turkey pork belly prosciutto.\n",
        "Beef ribs swine bresaola landjaeger tri-tip kevin rump meatball ground round shankle strip steak beef boudin filet mignon pork chop.\n",
        "Tail turkey hamburger pork salami pastrami porchetta landjaeger pork loin pig spare ribs drumstick.\n",
        "Ribeye pancetta meatloaf filet mignon, chicken ham hock sausage meatball spare ribs beef ribs venison capicola t-bone sirloin.\n",
        "Ground round flank shank cupim.\n",
        "Alcatra andouille hamburger, fatback flank sausage jowl tail doner rump filet mignon ham cow prosciutto.\n",
        "Sausage filet mignon tail landjaeger turducken.\n",
        "Jerky bacon porchetta meatball shoulder landjaeger.\n",
        "Pig cow turducken bacon beef frankfurter.\n"
    };

    int i;
    char tmp[256];
    for(i=0; i<count; i++) {
        const char *line = lines[i % 10];
        int n = snprintf(tmp, sizeof(tmp), "%04u%s", (unsigned) strlen(line), line);
        outAppend(c, tmp, n);
    }
}

static void appendBlock(Conn *c)        // K_BLOCKS: 16 bit counter pattern
{
    uint8_t *b = malloc(c->blockLen + 1);
    uint32_t i;
    for(i=0; i<c->blockLen; i += 2) {
        b[i]     = (uint8_t) ((i / 2) >> 8);
        b[i + 1] = (uint8_t) (i / 2);
    }
    outAppend(c, b, c->blockLen);
    free(b);
}

//---------------------------------------------
static Conn *addConn(int fd, int kind, const char *peer)
{
    Conn *c = calloc(1, sizeof(Conn));
    c->fd   = fd;
    c->kind = kind;
    snprintf(c->peer, sizeof(c->peer), "%s", peer);

    switch(kind) {
        case K_BLOCKS: c->hdrNeed = 8; break;
        case K_LINES:  c->hdrNeed = 2; break;
        case K_CLOSE:  c->hdrNeed = 2; break;
        case K_SCEN:   c->hdrNeed = 8; break;
        case K_CONNBK: c->hdrNeed = 6; break;
        default:       c->hdrNeed = 0; break;
    }

    if(kind == K_SINK) {
        c->counting = 1;
        c->scenCmd  = 'C';
        snprintf(report, sizeof(report), "- 0\n");
    }

    // K_REPORT: answered in sendReports(), after this round's reads - so a just finished count is in the report

    c->next = conns;
    conns   = c;
    return c;
}

static void closeConn(Conn *c, int reset, const char *why)
{
    if(c->fd < 0) return;

    if(reset) {
        struct linger lg = { 1, 0 };        // close() sends RST
        setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }

    logmsg("[%s %s] %s (%s), out pending %zu", kindName[c->kind], c->peer, reset ? "reset" : "closed", why,
         c->outLen - c->outOff);
    close(c->fd);
    c->fd = -1;
}

static void finishCounting(Conn *c)
{
    c->counting = 0;
    snprintf(report, sizeof(report), "%c %ld\n", c->scenCmd, c->count);
    logmsg("[%s %s] EOF after %ld bytes, report: %c %ld", kindName[c->kind], c->peer, c->count, c->scenCmd, c->count);
}

//---------------------------------------------
static void startScenario(Conn *c, int64_t now)
{
    char     cmd  = (char) c->hdr[0];
    uint16_t arg1 = (c->hdr[2] << 8) | c->hdr[3];
    uint32_t arg2 = ((uint32_t) c->hdr[4] << 24) | (c->hdr[5] << 16) | (c->hdr[6] << 8) | c->hdr[7];

    c->scenCmd = cmd;
    snprintf(report, sizeof(report), "- 0\n");
    logmsg("[scen %s] scenario %c arg1=%u arg2=%u", c->peer, cmd, arg1, arg2);

    switch(cmd) {
        case 'R':
            closeConn(c, 1, "scenario R");
            break;

        case 'F':
        case 'X':
            c->blockLen = arg2;
            c->eventAt  = now + arg1;
            if(c->eventAt == 0) c->eventAt = 1;
            break;

        case 'H':
            shutdown(c->fd, SHUT_WR);
            c->shutWr   = 1;
            c->counting = 1;
            break;

        case 'S':
            c->stallUntil = now + arg1;
            c->counting   = 1;
            break;

        case 'C':
            c->counting = 1;
            break;

        default:
            logmsg("[scen %s] unknown scenario '%c'", c->peer, cmd);
            closeConn(c, 0, "unknown scenario");
            break;
    }
}

static void connectBack(Conn *c)
{
    char host[32];
    int port = (c->hdr[4] << 8) | c->hdr[5];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", c->hdr[0], c->hdr[1], c->hdr[2], c->hdr[3]);

    closeConn(c, 0, "connect back request done");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    inet_pton(AF_INET, host, &sa.sin_addr);

    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));      // limits the blocking connect()

    if(connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        logmsg("[connbk] connect to %s:%d failed: %s", host, port, strerror(errno));
        close(fd);
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setNonBlock(fd);

    char peer[48];
    snprintf(peer, sizeof(peer), "%s:%d", host, port);
    addConn(fd, K_ECHO, peer);
    logmsg("[connbk] connected to %s, echoing", peer);
}

// header complete - act on it
static void configured(Conn *c, int64_t now)
{
    c->configured = 1;

    switch(c->kind) {
        case K_BLOCKS:
            c->blocksLeft = (c->hdr[0] << 8) | c->hdr[1];
            c->blockLen   = ((uint32_t) c->hdr[2] << 24) | (c->hdr[3] << 16) | (c->hdr[4] << 8) | c->hdr[5];
            c->blockPause = (c->hdr[6] << 8) | c->hdr[7];
            c->eventAt    = now;
            logmsg("[blocks %s] %d blocks x %u B, pause %d ms", c->peer, c->blocksLeft, c->blockLen, c->blockPause);
            break;

        case K_LINES: {
            int n = (c->hdr[0] << 8) | c->hdr[1];
            appendLines(c, n);
            c->closeAfterFlush = 1;
            logmsg("[lines %s] %d lines, %zu B", c->peer, n, c->outLen);
            break;
        }

        case K_CLOSE: {
            int ms = (c->hdr[0] << 8) | c->hdr[1];
            c->eventAt = now + ms;
            logmsg("[close %s] will close in %d ms", c->peer, ms);
            break;
        }

        case K_SCEN:
            startScenario(c, now);
            break;

        case K_CONNBK:
            connectBack(c);
            break;
    }
}

static void handleRead(Conn *c, int64_t now)
{
    uint8_t buf[65536];
    ssize_t n = read(c->fd, buf, sizeof(buf));

    if(n < 0) {
        if(errno == EAGAIN || errno == EINTR) return;
        if(c->counting) finishCounting(c);
        closeConn(c, 0, strerror(errno));
        return;
    }

    if(n == 0) {                                // EOF
        if(c->counting) finishCounting(c);
        if(c->outOff < c->outLen) {             // still sending (lines, ...) - finish that, then close
            c->closeAfterFlush = 1;
            shutdown(c->fd, SHUT_RD);
            return;
        }
        closeConn(c, 0, "EOF from client");
        return;
    }

    ssize_t pos = 0;
    while(!c->configured && c->hdrNeed > 0 && pos < n) {    // header first
        c->hdr[c->hdrLen++] = buf[pos++];
        if(c->hdrLen == c->hdrNeed) {
            configured(c, now);
            if(c->fd < 0) return;
        }
    }

    ssize_t rest = n - pos;
    if(rest <= 0) return;

    if(c->kind == K_ECHO) {
        outAppend(c, buf + pos, rest);
    } else if(c->counting) {
        c->count += rest;
    }                                           // others: ignore extra data
}

static void handleWrite(Conn *c)
{
    while(c->outOff < c->outLen) {
        ssize_t n = write(c->fd, c->out + c->outOff, c->outLen - c->outOff);
        if(n < 0) {
            if(errno == EAGAIN || errno == EINTR) return;
            closeConn(c, 0, strerror(errno));
            return;
        }
        c->outOff += n;
    }

    c->outOff = c->outLen = 0;

    if(c->closeAfterFlush) {
        closeConn(c, c->closeAfterFlush == 2, "all sent");
    }
}

static void handleEvent(Conn *c, int64_t now)
{
    c->eventAt = 0;

    switch(c->kind) {
        case K_BLOCKS:
            if(c->blocksLeft > 0) {
                appendBlock(c);
                c->blocksLeft--;
                c->eventAt = c->blocksLeft > 0 ? now + c->blockPause : 0;
            }
            if(c->blocksLeft == 0) {
                c->closeAfterFlush = 1;
            }
            break;

        case K_CLOSE:
            closeConn(c, 0, "close time");
            break;

        case K_SCEN:                            // F / X: send the data now, then close / reset
            appendPattern(c, c->blockLen);
            c->closeAfterFlush = (c->scenCmd == 'X') ? 2 : 1;
            logmsg("[scen %s] sending %u B, then %s", c->peer, c->blockLen, c->scenCmd == 'X' ? "reset" : "close");
            break;
    }
}

// a scenario or count still running? then the report isn't final yet, "- 0" tells the client to ask again
static int scenarioRunning(void)
{
    Conn *c;
    for(c = conns; c; c = c->next) {
        if(c->fd < 0) continue;
        if(c->counting) return 1;
        if(c->kind == K_SCEN && (!c->configured || c->eventAt)) return 1;
    }
    return 0;
}

static void sendReports(void)
{
    Conn *c;
    int running = scenarioRunning();

    for(c = conns; c; c = c->next) {
        if(c->fd < 0 || c->kind != K_REPORT || c->closeAfterFlush) continue;

        const char *r = running ? "- 0\n" : report;
        outAppend(c, r, strlen(r));
        c->closeAfterFlush = 1;
        logmsg("[report %s] sending: %.*s", c->peer, (int) strlen(r) - 1, r);
        handleWrite(c);
    }
}

//---------------------------------------------
// UDP
typedef struct {
    struct sockaddr_in peer;
    int     blocksLeft;
    uint32_t blockLen;
    int     pause;
    int64_t next;
} UdpBlocks;

static UdpBlocks udpBlocks;

static void handleUdp(int fd, int idx, int64_t now)
{
    uint8_t buf[65536];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &peer, &plen);
    if(n < 0) return;

    char ps[32];
    snprintf(ps, sizeof(ps), "%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

    switch(idx) {
        case 4:
            logmsg("[udp echo %s] %zd B", ps, n);
            sendto(fd, buf, n, 0, (struct sockaddr *) &peer, plen);
            break;

        case 5:
            if(n >= 8) {
                udpBlocks.peer       = peer;
                udpBlocks.blocksLeft = (buf[0] << 8) | buf[1];
                udpBlocks.blockLen   = ((uint32_t) buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];
                udpBlocks.pause      = (buf[6] << 8) | buf[7];
                udpBlocks.next       = now;
                logmsg("[udp blocks %s] %d x %u B", ps, udpBlocks.blocksLeft, udpBlocks.blockLen);
            }
            break;

        case 6:
            if(n >= 2) {
                Conn tmp;
                memset(&tmp, 0, sizeof(tmp));
                appendLines(&tmp, (buf[0] << 8) | buf[1]);
                size_t len = tmp.outLen > 65000 ? 65000 : tmp.outLen;
                sendto(fd, tmp.out, len, 0, (struct sockaddr *) &peer, plen);
                logmsg("[udp lines %s] %d lines, %zu B", ps, (buf[0] << 8) | buf[1], len);
                free(tmp.out);
            }
            break;

        case 7:
            logmsg("[udp close %s] %zd B ignored", ps, n);
            break;
    }
}

static void udpBlocksTick(int fd, int64_t now)
{
    if(udpBlocks.blocksLeft <= 0 || now < udpBlocks.next) return;

    Conn tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.blockLen = udpBlocks.blockLen;
    appendBlock(&tmp);
    sendto(fd, tmp.out, tmp.outLen, 0, (struct sockaddr *) &udpBlocks.peer, sizeof(udpBlocks.peer));
    free(tmp.out);

    udpBlocks.blocksLeft--;
    udpBlocks.next = now + udpBlocks.pause;
}

//---------------------------------------------
int main(int argc, char *argv[])
{
    int i;
    for(i=1; i<argc; i++) {
        if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) portBase = atoi(argv[++i]);
        else if(strcmp(argv[i], "-q") == 0)            quiet = 1;
        else {
            fprintf(stderr, "usage: %s [-p base_port] [-q]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    struct { int offset; int kind; int rcvBuf; int fd; } tcpL[] = {
        { 0,  K_ECHO,   0,    -1 },
        { 1,  K_BLOCKS, 0,    -1 },
        { 2,  K_LINES,  0,    -1 },
        { 3,  K_CLOSE,  0,    -1 },
        { 8,  K_SCEN,   2048, -1 },     // small window, so a stalled reader fills up quickly
        { 10, K_REPORT, 0,    -1 },
        { 11, K_SINK,   0,    -1 },
        { 20, K_CONNBK, 0,    -1 },
    };
    int tcpCount = sizeof(tcpL) / sizeof(tcpL[0]);
    int udpFd[8];

    for(i=0; i<tcpCount; i++) {
        tcpL[i].fd = makeListener(portBase + tcpL[i].offset, 1, tcpL[i].rcvBuf);
    }
    for(i=4; i<8; i++) {
        udpFd[i] = makeListener(portBase + i, 0, 0);
    }

    printf("sting_server: TCP %d-%d, %d, %d, %d, %d, UDP %d-%d (nothing on %d)\n", portBase, portBase + 3, portBase + 8,
           portBase + 10, portBase + 11, portBase + 20, portBase + 4, portBase + 7, portBase + 9);
    fflush(stdout);

    while(1) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int maxFd = -1;
        int64_t now = nowMs();
        int64_t wake = now + 1000;

        for(i=0; i<tcpCount; i++) {
            FD_SET(tcpL[i].fd, &rfds);
            if(tcpL[i].fd > maxFd) maxFd = tcpL[i].fd;
        }
        for(i=4; i<8; i++) {
            FD_SET(udpFd[i], &rfds);
            if(udpFd[i] > maxFd) maxFd = udpFd[i];
        }
        if(udpBlocks.blocksLeft > 0 && udpBlocks.next < wake) wake = udpBlocks.next;

        Conn *c;
        for(c = conns; c; c = c->next) {
            if(c->fd < 0) continue;

            if(c->stallUntil && now < c->stallUntil) {
                if(c->stallUntil < wake) wake = c->stallUntil;
            } else {
                FD_SET(c->fd, &rfds);
            }
            if(c->outOff < c->outLen || (c->closeAfterFlush && c->outLen == 0)) {
                FD_SET(c->fd, &wfds);
            }
            if(c->eventAt && c->eventAt < wake) wake = c->eventAt;
            if(c->fd > maxFd) maxFd = c->fd;
        }

        int64_t waitMs = wake - now;
        if(waitMs < 0) waitMs = 0;
        struct timeval tv = { waitMs / 1000, (waitMs % 1000) * 1000 };

        int r = select(maxFd + 1, &rfds, &wfds, NULL, &tv);
        if(r < 0) {
            if(errno == EINTR) continue;
            perror("select");
            return 1;
        }

        now = nowMs();

        for(i=0; i<tcpCount; i++) {         // new connections
            if(!FD_ISSET(tcpL[i].fd, &rfds)) continue;

            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int fd = accept(tcpL[i].fd, (struct sockaddr *) &sa, &sl);
            if(fd < 0) continue;

            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            setNonBlock(fd);

            char peer[32];
            snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
            logmsg("[%s %s] connected to port %d", kindName[tcpL[i].kind], peer, portBase + tcpL[i].offset);
            addConn(fd, tcpL[i].kind, peer);
        }

        for(i=4; i<8; i++) {
            if(FD_ISSET(udpFd[i], &rfds)) handleUdp(udpFd[i], i, now);
        }
        udpBlocksTick(udpFd[5], now);

        for(c = conns; c; c = c->next) {
            if(c->fd >= 0 && FD_ISSET(c->fd, &rfds)) handleRead(c, now);
            if(c->fd >= 0 && FD_ISSET(c->fd, &wfds)) handleWrite(c);
            if(c->fd >= 0 && c->eventAt && now >= c->eventAt) handleEvent(c, now);
            if(c->fd >= 0 && c->outOff < c->outLen) handleWrite(c);     // try right away, don't wait for select
        }

        sendReports();

        Conn **pp = &conns;                 // drop closed connections
        while(*pp) {
            if((*pp)->fd < 0) {
                Conn *dead = *pp;
                *pp = dead->next;
                free(dead->out);
                free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}
