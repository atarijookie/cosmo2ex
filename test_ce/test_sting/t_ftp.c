// 0x81xx - FTP download speed: a minimal FTP client without GEM - to see what the stack can do when the app doesn't
// wait in an event loop (MG-FTP / aFTP read 4 kB per GEM event loop round). Fixed server / port / file, no config:
// the FTP server runs on the same host as sting_server (SERVER_ADDR), port FTP_PORT, anonymous login.
#include "stest.h"

#define FTP_PORT        2122
#define FTP_FILE        "TEST4M.BIN"
#define FTP_LOCAL       "C:\\TEST4M.BIN"
#define WRITE_BUF_SIZE  (32 * 1024)         // Fwrite() in big blocks - each Fwrite() is one command to the host
#define READ_BLOCK      (16 * 1024)

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

// send a command line (CR LF added)
static int16 ftpSend(int16 ctrl, const char *cmd)
{
    char line[128];
    int len = ce_snprintf(line, sizeof(line), "%s\r\n", cmd);
    return sendAll(ctrl, 1, line, len, TMO(5));
}

// read the reply - multi line replies ("123-..." up to "123 ...") too. Returns the code, < 0 on error / timeout.
// The last line is in 'last'.
static int ftpReply(int16 ctrl, char *last, int lastSize)
{
    uint32_t end = ticks() + TMO(15) * 200;
    char line[256];
    int code = -1, multi = 0;

    while(ticks() < end) {
        int16 res = CNgets(ctrl, line, sizeof(line), '\n');

        if(res == E_NODATA) {
            continue;
        }
        if(res < 0) {
            return res;
        }

        if(res > 0 && line[res - 1] == '\r') {     // CR LF line ends
            line[res - 1] = 0;
        }

        int isCodeLine = (line[0] >= '0' && line[0] <= '9' && line[1] >= '0' && line[1] <= '9' &&
                          line[2] >= '0' && line[2] <= '9');
        int thisCode   = isCodeLine ? (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0') : -1;

        if(!multi && isCodeLine && line[3] == '-') {        // start of a multi line reply
            multi = 1;
            code  = thisCode;
            continue;
        }

        if(isCodeLine && line[3] != '-' && (!multi || thisCode == code)) {    // the final line
            if(last) {
                strncpy(last, line, lastSize - 1);
                last[lastSize - 1] = 0;
            }
            return thisCode;
        }
    }

    return -1000;                                   // timeout
}

static int ftpCommand(int16 ctrl, const char *cmd, char *reply, int replySize)
{
    if(ftpSend(ctrl, cmd) != E_NORMAL) {
        return -1001;
    }
    return ftpReply(ctrl, reply, replySize);
}

// PASV -> "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" -> data connection
static int16 ftpOpenData(int16 ctrl)
{
    char reply[160];

    if(ftpCommand(ctrl, "PASV", reply, sizeof(reply)) != 227) {
        ce_printf("PASV failed: %s\n", reply);
        return -1;
    }

    char *p = reply;
    while(*p && *p != '(') p++;
    if(!*p) {
        return -1;
    }
    p++;

    uint32_t n[6];
    int i;
    for(i=0; i<6; i++) {
        n[i] = 0;
        while(*p >= '0' && *p <= '9') {
            n[i] = n[i] * 10 + (*p - '0');
            p++;
        }
        if(*p == ',' || *p == ')') p++;
    }

    uint32 host = (n[0] << 24) | (n[1] << 16) | (n[2] << 8) | n[3];
    uint16 port = (n[4] << 8) | n[5];

    int16 waitRes;
    int16 data = openTcp(host, port, 16000, TMO(5), &waitRes);
    if(data < 0 || waitRes != E_NORMAL) {
        ce_printf("data connection to %d.%d.%d.%d:%d failed: %d\n", (int) n[0], (int) n[1], (int) n[2], (int) n[3],
                  (int) port, data < 0 ? data : waitRes);
        if(data >= 0) TCP_close(data, 0, 0);
        return -1;
    }

    return data;
}

static void showProgress(uint32_t bytes, uint32_t total, uint32_t start)
{
    uint32_t ms = (ticks() - start) * 5;
    if(ms == 0) ms = 1;

    if(total) {
        ce_printf("\r %d / %d kB  %d kB/s  %d %%   ", (int) (bytes >> 10), (int) (total >> 10), (int) (bytes / ms),
                  (int) ((bytes / 1024) * 100 / ((total / 1024) ? (total / 1024) : 1)));
    } else {
        ce_printf("\r %d kB  %d kB/s   ", (int) (bytes >> 10), (int) (bytes / ms));
    }
}

void testFtp(void)
{
    char reply[160];
    int code;

    rec_test(0x8100, "FTP download " FTP_FILE " to " FTP_LOCAL);

    int16 waitRes;
    int16 ctrl = openTcp(SERVER_ADDR, FTP_PORT, 2000, TMO(5), &waitRes);
    if(ctrl < 0 || waitRes != E_NORMAL) {
        ce_printf("can't connect to the FTP server on port %d: %d\n", FTP_PORT, ctrl < 0 ? ctrl : waitRes);
        rec_x("connect", ctrl < 0 ? ctrl : waitRes);
        if(ctrl >= 0) TCP_close(ctrl, 0, 0);
        return;
    }

    code = ftpReply(ctrl, reply, sizeof(reply));
    ce_printf("%s\n", reply);

    code = ftpCommand(ctrl, "USER anonymous", reply, sizeof(reply));
    if(code == 331) {
        code = ftpCommand(ctrl, "PASS st@cosmosex", reply, sizeof(reply));
    }
    ce_printf("login: %s\n", reply);
    if(code != 230) {
        rec_x("login", code);
        goto quit;
    }

    ftpCommand(ctrl, "TYPE I", reply, sizeof(reply));

    //----------------------------------
    // directory
    int16 data = ftpOpenData(ctrl);
    if(data < 0) {
        goto quit;
    }

    code = ftpCommand(ctrl, "LIST", reply, sizeof(reply));
    if(code != 150 && code != 125) {
        ce_printf("LIST failed: %s\n", reply);
        TCP_close(data, 0, 0);
        goto quit;
    }

    int32_t listBytes = 0, listLines = 0;
    uint32_t end = ticks() + TMO(10) * 200;
    char line[256];
    ce_printf("directory:\n");
    while(ticks() < end) {
        int16 res = CNgets(data, line, sizeof(line), '\n');
        if(res == E_NODATA) continue;
        if(res < 0) break;                          // E_EOF - the listing is complete

        listBytes += res + 1;
        if(listLines < 10) {
            ce_printf("  %s\n", line);
        }
        listLines++;
    }
    TCP_close(data, 0, 0);
    ftpReply(ctrl, reply, sizeof(reply));           // 226
    ce_printf("%d entries, %d bytes\n", (int) listLines, (int) listBytes);
    rec_f("listlines", listLines);

    //----------------------------------
    // size, then the download
    uint32_t total = 0;
    if(ftpCommand(ctrl, "SIZE " FTP_FILE, reply, sizeof(reply)) == 213) {
        char *p = reply + 4;
        while(*p >= '0' && *p <= '9') {
            total = total * 10 + (*p - '0');
            p++;
        }
    }

    uint8_t *wbuf = (uint8_t *) Malloc(WRITE_BUF_SIZE);
    if(!wbuf) {
        ce_printf("no RAM for the write buffer\n");
        goto quit;
    }

    int32_t f = Fcreate(FTP_LOCAL, 0);
    if(f < 0) {
        ce_printf("can't create %s: %d\n", FTP_LOCAL, (int) f);
        Mfree(wbuf);
        goto quit;
    }

    data = ftpOpenData(ctrl);
    if(data < 0) {
        Fclose((int16_t) f);
        Mfree(wbuf);
        goto quit;
    }

    code = ftpCommand(ctrl, "RETR " FTP_FILE, reply, sizeof(reply));
    if(code != 150 && code != 125) {
        ce_printf("RETR failed: %s\n", reply);
        TCP_close(data, 0, 0);
        Fclose((int16_t) f);
        Mfree(wbuf);
        goto quit;
    }

    ce_printf("downloading %s (%d kB) to %s\n", FTP_FILE, (int) (total >> 10), FTP_LOCAL);

    uint32_t bytes = 0, inBuf = 0, writeTicks = 0, writes = 0;
    uint32_t start = ticks(), lastShow = start, lastData = start;
    int16 err = 0;

    while(1) {
        int32_t avail = CNbyte_count(data);

        if(avail == E_NODATA || avail == 0) {
            uint32_t now = ticks();
            if(now - lastData > TMO(30) * 200) {    // nothing for 30 s
                err = -1002;
                break;
            }
            if(now - lastShow >= 200) {
                showProgress(bytes, total, start);
                lastShow = now;
            }
            continue;
        }

        if(avail < 0) {                             // E_EOF - all of it came, or an error
            if(avail != E_EOF) err = avail;
            break;
        }

        int32_t want = WRITE_BUF_SIZE - inBuf;      // as much as fits into the write buffer, at most READ_BLOCK
        if(want > READ_BLOCK) want = READ_BLOCK;
        if(want > avail)      want = avail;

        int16 got = CNget_block(data, wbuf + inBuf, (int16) want);
        if(got == E_NODATA) {
            continue;
        }
        if(got < 0) {
            err = got;
            break;
        }

        inBuf   += got;
        bytes   += got;
        lastData = ticks();

        if(inBuf >= WRITE_BUF_SIZE) {
            uint32_t w0 = ticks();
            Fwrite((int16_t) f, inBuf, wbuf);
            writeTicks += ticks() - w0;
            writes++;
            inBuf = 0;
        }

        if(lastData - lastShow >= 200) {            // once a second
            showProgress(bytes, total, start);
            lastShow = lastData;
        }
    }

    if(inBuf > 0) {                                 // the rest
        uint32_t w0 = ticks();
        Fwrite((int16_t) f, inBuf, wbuf);
        writeTicks += ticks() - w0;
        writes++;
    }

    uint32_t ms = (ticks() - start) * 5;
    if(ms == 0) ms = 1;

    Fclose((int16_t) f);
    TCP_close(data, 0, 0);
    Mfree(wbuf);

    showProgress(bytes, total, start);
    code = ftpReply(ctrl, reply, sizeof(reply));    // 226 Transfer complete
    ce_printf("\n%s\n", reply);

    uint32_t netMs = ms - writeTicks * 5;
    if(netMs == 0) netMs = 1;

    ce_printf("%d bytes in %d ms: %d kB/s, %d writes took %d ms - without them %d kB/s\n", (int) bytes, (int) ms,
              (int) (bytes / ms), (int) writes, (int) (writeTicks * 5), (int) (bytes / netMs));
    if(err) {
        ce_printf("ended with error %d\n", err);
    }

    rec_x("complete", total == 0 || bytes == total);
    rec_f("bytes", bytes);
    rec_f("ms", ms);
    rec_f("kBps", bytes / ms);
    rec_f("writems", writeTicks * 5);
    rec_f("netkBps", bytes / netMs);
    rec_f("err", err);

quit:
    ftpCommand(ctrl, "QUIT", reply, sizeof(reply));
    TCP_close(ctrl, 0, 0);
}
