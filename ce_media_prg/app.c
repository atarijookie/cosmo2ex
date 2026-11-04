// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Shared main of CE_VIDEO.TTP and CE_AUDIO.TTP: command line, keys, what's shown in text mode.
//
//   CE_VIDEO.TTP [options] file      CE_AUDIO.TTP [options] file
//   options:  -l<ms>   buffer (latency), default 1000
//             -f<fps>  most frames per second (video)
//             -s<sec>  start at this second
//             -n       no sound
//             -d       debug log to X:\CE_MEDIA.LOG (X = drive of the file)
//             -p<n>    VBLs between a new screen address and its palette (default 1)
//   keys:     Esc / Q quit, Space pause, Left / Right -+10 s, Down / Up -+60 s
//
// The file must be on a CosmosEx translated drive - the host reads and converts it.

#include <mint/osbind.h>
#include <mint/basepage.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "med.h"
#include "player.h"
#include "app.h"
#include "ui.h"
#include "dbg.h"

extern BASEPAGE *_base;
extern const char buildStamp[];

#define KEY_ESC     0x01
#define KEY_Q       0x10
#define KEY_SPACE   0x39
#define KEY_UP      0x48
#define KEY_LEFT    0x4b
#define KEY_RIGHT   0x4d
#define KEY_DOWN    0x50

static void print(const char *s)
{
    (void) Cconws(s);
}

static void printNum(uint32_t v, int minDigits)
{
    char buf[12];
    int  i = sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
        minDigits--;
    } while(v > 0 || minDigits > 0);
    print(buf + i);
}

static void waitKey(void)
{
    print("\r\nPress any key.");
    Cnecin();
}

static int fail(const char *what)
{
    print("\r\n");
    print(what);
    print("\r\n");
    waitKey();
    return 1;
}

// IKBD: stop / start sending mouse packets - every move is an interrupt, and the pointer is drawn in the VBL
static void mouseReports(uint8_t on)
{
    static char disable[1] = { 0x12 };                      // disable mouse
    static char relative[1] = { 0x08 };                     // relative mouse mode - what TOS uses
    Ikbdws(0, on ? relative : disable);                     // count - 1
}

static void progress(const char *msg)
{
    char line[34];                                          // fits 40 columns (ST low) with the Esc hint
    strncpy(line, msg, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    print("\r\33K");
    print(line);
    print(" (Esc)");
}

static uint32_t parseNum(const char **p)
{
    uint32_t v = 0;
    while(**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

// full Atari path: with drive and from the root - the host has no current directory of this program
static void fullPath(const char *in, char *out)
{
    char cur[128];
    int  drive;

    if(in[0] && in[1] == ':') {
        drive = (in[0] & 0x5f) - 'A';
        in += 2;
    } else {
        drive = Dgetdrv();
    }
    out[0] = 'A' + drive;
    out[1] = ':';
    out[2] = 0;

    if(in[0] != '\\') {
        cur[0] = 0;
        Dgetpath(cur, drive + 1);
        strcat(out, cur);
        if(out[strlen(out) - 1] != '\\') {
            strcat(out, "\\");
        }
    }
    strcat(out, in);
}

int appMain(uint8_t wantVideo)
{
    PlayerOpts  o;
    char        args[128], path[256];
    const char *p;
    uint8_t     noSound = FALSE, quit = FALSE, video, debug = FALSE;
    int         len, i;
    uint32_t    lastStatus = 0, dbgLast = 0;
    int         dbgCount = 0;

    memset(&o, 0, sizeof(o));
    o.latencyMs = 1000;
    o.palDelay  = 1;

    len = (uint8_t) _base->p_cmdlin[0];                     // command line: length, then the text
    if(len > 126) {
        len = 0;
    }
    memcpy(args, _base->p_cmdlin + 1, len);
    args[len] = 0;

    print("\33E");
    print(wantVideo ? "CosmosEx video player\r\n" : "CosmosEx audio player\r\n");

    p = args;
    for(;;) {
        while(*p == ' ') {
            p++;
        }
        if(*p != '-') {
            break;
        }
        p++;
        switch(*p++ | 0x20) {
            case 'l':   o.latencyMs = (uint16_t) parseNum(&p);  break;
            case 'f':   o.maxFps    = (uint8_t) parseNum(&p);   break;
            case 's':   o.startMs   = parseNum(&p) * 1000;      break;
            case 'p':   o.palDelay  = (uint8_t) parseNum(&p);   break;
            case 'n':   noSound     = TRUE;                     break;
            case 'd':   debug       = TRUE;                     break;
        }
        while(*p && *p != ' ') {
            p++;
        }
    }
    for(i = strlen(p); i > 0 && (p[i - 1] == ' ' || p[i - 1] == '\r' || p[i - 1] == '\n'); i--) {
    }
    if(i == 0) {
        print("\r\nusage: ");
        print(wantVideo ? "CE_VIDEO" : "CE_AUDIO");
        print(" [-l<ms>] [-f<fps>] [-s<sec>] [-n] [-d] file\r\n");
        print("The file must be on a CosmosEx translated drive.\r\n");
        print("Keys: Esc quit, Space pause, arrows seek.\r\n");
        print("Build ");
        print(buildStamp);
        print("\r\n");
        waitKey();
        return 1;
    }
    memcpy(args, p, i);
    args[i] = 0;
    fullPath(args, path);
    print(path);
    print("\r\n");

    dbgStart(debug ? path[0] : 0);
    dbgLog(wantVideo ? "CE_VIDEO build" : "CE_AUDIO build");
    dbgLog(buildStamp);
    dbgLog(path);
    if(!playerDetect(&o, wantVideo, noSound)) {
        return fail(playerError);
    }
    if(o.aout == MED_AOUT_NONE && !wantVideo) {
        return fail("Nothing to play without sound");
    }
    if(!playerOpen(&o, path, progress)) {
        return fail(playerError);
    }

    video = playerHasVideo();
    print("\r\33K");
    print(playerTitle);
    print("\r\n");
    if(wantVideo && !video) {
        print("(no video in this file - sound only)\r\n");
    }

    if(!playerStart()) {
        playerStop();                                       // gives back what it took, closes the stream on the host
        return fail(playerError);
    }
    dbgLog("started, mouse off");
    mouseReports(FALSE);                                    // mouse moves interrupt the ST - not needed while playing
    if(!video) {
        uiStart(path);
        dbgLog("screen drawn");
    }

    while(!quit) {
        if(!playerPoll()) {
            break;
        }

        if(debug && dbgCount < 10 && getTicksAsUser() - dbgLast >= 100) {     // the first 5 s: is it going on?
            dbgLast = getTicksAsUser();
            dbgCount++;
            dbgLogNum("position ms", playerPositionMs());
        }

        while(Bconstat(2)) {
            uint8_t scan = (uint8_t) (Bconin(2) >> 16);
            switch(scan) {
                case KEY_ESC:
                case KEY_Q:     quit = TRUE;                        break;
                case KEY_SPACE: playerPause(!playerPaused());       break;
                case KEY_LEFT:  playerSeek(-10000);                 break;
                case KEY_RIGHT: playerSeek(10000);                  break;
                case KEY_DOWN:  playerSeek(-60000);                 break;
                case KEY_UP:    playerSeek(60000);                  break;
            }
        }

        if(!video) {
            if(getTicksAsUser() - lastStatus >= 20) {           // looked at 10 times a second, drawn when it changed
                lastStatus = getTicksAsUser();
                uiUpdate();
            }
        }
    }

    playerStop();
    dbgLog("player stopped, mouse on");
    mouseReports(TRUE);
    if(!video) {
        uiEnd();
    }

    dbgLog("printing the stats");
    print("\r\n");
    if(playerError[0]) {
        return fail(playerError);
    }
    if(video) {
        print("frames ");
        printNum(playerStats.frames, 1);
        print(", late ");
        printNum(playerStats.skipped, 1);
        print(", ");
    }
    print("buffer ran out ");
    printNum(playerStats.underruns, 1);
    print("x, chunk buffers ");
    printNum(playerStats.chunkBuffers, 1);
    print("\r\n");
    dbgLog("end");
    return 0;
}
