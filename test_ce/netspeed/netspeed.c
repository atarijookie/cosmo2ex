// netspeed - TCP speed test for MiNTnet (inet4.xdd + CE_ETH.XIF), against netspeed_server.py on a PC.
// Only network: no GEM, no GlueSTiK, nothing written to disk. Plain sockets, so it also builds with gcc on Linux.
//
// usage: netspeed <server IP> [get|put|both|check] [kB] [port] - see README.md
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_KB      4096
#define DEFAULT_PORT    5001
#define BUF_SIZE        (32 * 1024)
#define DOT_BYTES       (256UL * 1024)
#define PATTERN_MOD     251                 // prime, so a lost or doubled block doesn't line up with the buffer size

static char buffer[BUF_SIZE];

static unsigned long nowMs(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((unsigned long) tv.tv_sec) * 1000UL + ((unsigned long) tv.tv_usec) / 1000UL;
}

static void showResult(const char *what, unsigned long bytes, unsigned long ms, unsigned long calls)
{
    if(ms == 0) {
        ms = 1;
    }

    unsigned long kBps = (bytes / 1024UL) * 1000UL / ms;    // kB first, so 32 bits are enough for up to 4 GB

    printf("\n%s: %lu bytes in %lu.%03lu s = %lu kB/s (%lu calls, avg %lu bytes)\n", what, bytes, ms / 1000UL, ms % 1000UL,
           kBps, calls, calls ? bytes / calls : 0UL);
}

static int openConnection(const struct sockaddr_in *server)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if(s < 0) {
        perror("socket");
        return -1;
    }

    if(connect(s, (const struct sockaddr *) server, sizeof(*server)) < 0) {
        perror("connect");
        close(s);
        return -1;
    }

    return s;
}

static int sendAll(int s, const char *data, unsigned long len)
{
    while(len > 0) {
        long r = send(s, data, len, 0);

        if(r <= 0) {
            perror("send");
            return -1;
        }

        data += r;
        len  -= r;
    }

    return 0;
}

static int sendCommand(int s, const char *cmd, unsigned long bytes)
{
    char line[40];
    int len = sprintf(line, "%s %lu\n", cmd, bytes);
    return sendAll(s, line, len);
}

static void fillPattern(char *bfr, unsigned long len, unsigned long pos)
{
    unsigned long i;
    for(i = 0; i < len; i++) {
        bfr[i] = (char) ((pos + i) % PATTERN_MOD);
    }
}

// returns the count of wrong bytes
static unsigned long checkPattern(const char *bfr, unsigned long len, unsigned long pos)
{
    unsigned long i, bad = 0;
    for(i = 0; i < len; i++) {
        if(bfr[i] != (char) ((pos + i) % PATTERN_MOD)) {
            bad++;
        }
    }
    return bad;
}

static int doGet(const struct sockaddr_in *server, unsigned long bytes, int check)
{
    int s = openConnection(server);
    if(s < 0) {
        return 1;
    }

    if(sendCommand(s, "GET", bytes)) {
        close(s);
        return 1;
    }

    unsigned long start = nowMs();
    unsigned long got = 0, calls = 0, bad = 0, nextDot = DOT_BYTES;

    while(got < bytes) {
        unsigned long want = bytes - got;
        long r = recv(s, buffer, want < BUF_SIZE ? want : BUF_SIZE, 0);

        if(r < 0) {
            perror("recv");
            break;
        }

        if(r == 0) {                                    // server closed the connection too soon
            break;
        }

        if(check) {
            bad += checkPattern(buffer, r, got);
        }

        got += r;
        calls++;

        if(got >= nextDot) {
            nextDot += DOT_BYTES;
            write(1, ".", 1);
        }
    }

    unsigned long ms = nowMs() - start;
    close(s);

    showResult(check ? "CHECK (ST <- server)" : "GET (ST <- server)", got, ms, calls);

    if(got != bytes) {
        printf("FAILED: expected %lu bytes, got %lu\n", bytes, got);
        return 1;
    }

    if(check) {
        printf("data check: %lu wrong bytes - %s\n", bad, bad ? "FAILED" : "OK");
    }

    return bad ? 1 : 0;
}

static int doPut(const struct sockaddr_in *server, unsigned long bytes)
{
    int s = openConnection(server);
    if(s < 0) {
        return 1;
    }

    if(sendCommand(s, "PUT", bytes)) {
        close(s);
        return 1;
    }

    // the pattern is filled before the clock starts and only for 1 buffer: its length is a multiple of PATTERN_MOD, so
    // the buffer repeats seamlessly, and filling doesn't count in the send speed
    unsigned long chunk = (BUF_SIZE / PATTERN_MOD) * PATTERN_MOD;
    fillPattern(buffer, chunk, 0);

    unsigned long start = nowMs();
    unsigned long sent = 0, calls = 0, nextDot = DOT_BYTES;

    while(sent < bytes) {
        unsigned long left = bytes - sent;

        if(sendAll(s, buffer, left < chunk ? left : chunk)) {
            close(s);
            return 1;
        }

        sent += left < chunk ? left : chunk;
        calls++;

        if(sent >= nextDot) {
            nextDot += DOT_BYTES;
            write(1, ".", 1);
        }
    }

    // wait for the answer of the server - only then all the data really left the ST
    char line[64];
    long got = 0;

    while(got < (long) sizeof(line) - 1) {
        long r = recv(s, line + got, sizeof(line) - 1 - got, 0);

        if(r <= 0) {
            break;
        }

        got += r;

        if(line[got - 1] == '\n') {
            break;
        }
    }

    line[got > 0 ? got : 0] = 0;

    unsigned long ms = nowMs() - start;
    close(s);

    showResult("PUT (ST -> server)", sent, ms, calls);

    unsigned long srvBytes = 0, srvBad = 1;

    if(sscanf(line, "OK %lu %lu", &srvBytes, &srvBad) != 2) {
        printf("FAILED: no answer from server\n");
        return 1;
    }

    printf("server got %lu bytes, %lu wrong - %s\n", srvBytes, srvBad, (srvBytes == bytes && !srvBad) ? "OK" : "FAILED");
    return (srvBytes == bytes && !srvBad) ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if(argc < 2) {
        printf("usage: netspeed <server IP> [get|put|both|check] [kB] [port]\n");
        printf("  get   - download (default size %d kB), nothing is checked, this is the speed test\n", DEFAULT_KB);
        printf("  put   - upload\n");
        printf("  both  - get, then put (default)\n");
        printf("  check - download and check every byte - slower, it tests the data, not the speed\n");
        return 1;
    }

    const char *mode    = (argc > 2) ? argv[2] : "both";
    unsigned long kb    = (argc > 3) ? strtoul(argv[3], NULL, 10) : DEFAULT_KB;
    unsigned long port  = (argc > 4) ? strtoul(argv[4], NULL, 10) : DEFAULT_PORT;

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family       = AF_INET;
    server.sin_port         = htons((unsigned short) port);
    server.sin_addr.s_addr  = inet_addr(argv[1]);

    if(server.sin_addr.s_addr == INADDR_NONE) {
        printf("'%s' is not an IP address\n", argv[1]);
        return 1;
    }

    if(kb == 0 || kb >= 4UL * 1024UL * 1024UL) {
        printf("size must be 1 kB to under 4 GB\n");
        return 1;
    }

    unsigned long bytes = kb * 1024UL;
    int res = 0;

    if(!strcmp(mode, "get") || !strcmp(mode, "both")) {
        res |= doGet(&server, bytes, 0);
    } else if(!strcmp(mode, "check")) {
        res |= doGet(&server, bytes, 1);
    } else if(strcmp(mode, "put")) {
        printf("unknown mode '%s'\n", mode);
        return 1;
    }

    if(!strcmp(mode, "put") || !strcmp(mode, "both")) {
        res |= doPut(&server, bytes);
    }

    return res;
}
