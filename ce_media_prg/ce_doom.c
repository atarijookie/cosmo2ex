// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// CE_DOOM.PRG - Doom for the Atari ST, played on the host: CosmosEx's host runs Doom (ce_media starts ce_doomgeneric
// with Freedoom or your own WAD), the ST shows its picture full screen and sends the keys - presses and releases,
// straight from the keyboard (keys.s). The playback engine of CE_VIDEO (player.c) in live mode: one frame per
// transfer, always the newest one.
//
//   keys:  arrows move / turn, Control fire, Space open, Shift run, Alternate + arrows strafe, , . strafe,
//          1 .. 7 weapons, Tab map, Esc Doom's menu (quit the game there), Help = F1
//          Undo - quit at once
//
//   CE_DOOM.PRG [-d] [-f<fps>] [-n] [-h]
//     -d  debug log to C:\CE_MEDIA.LOG     -f  most frames per second (default: 20)
//     -n  no sound                         -h  sound at half the rate (YM 7.9 kHz, DMA 12.5 kHz) - on a plain ST
//                                              the YM interrupt then takes a quarter of the CPU instead of half
//
// Sound: Doom's sound effects (no music yet), mixed on the host. DMA sound on STE / TT / Falcon, the YM on a ST.

#include <mint/osbind.h>
#include <mint/basepage.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "med.h"
#include "player.h"
#include "dbg.h"

extern BASEPAGE *_base;
extern const char buildStamp[];

// keys.s
void             keyIsr(void);
extern uint8_t   keyRing[64];
extern volatile uint8_t keyHead;
static uint8_t   keyTail;
static void     *oldIkbdsys;

#define KEY_UNDO    0x61

static void print(const char *s)
{
    (void) Cconws(s);
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

static void progress(const char *msg)
{
    char line[34];
    strncpy(line, msg, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    print("\r\33K");
    print(line);
    print(" (Esc)");
}

// IKBD: mouse packets off while playing (each move is an interrupt), back to relative mode after
static void mouseReports(uint8_t on)
{
    static char disable[1]  = { 0x12 };
    static char relative[1] = { 0x08 };
    Ikbdws(0, on ? relative : disable);
}

// Kbdvbase(): midivec, vkbderr, vmiderr, statvec, mousevec, clockvec, joyvec, midisys, ikbdsys - the 9th pointer
static void keysOnSuper(void)
{
    void **kv = (void **) Kbdvbase();
    keyTail = keyHead;
    oldIkbdsys = kv[8];
    kv[8] = (void *) keyIsr;
}

static void keysOffSuper(void)
{
    void **kv = (void **) Kbdvbase();
    kv[8] = oldIkbdsys;
}

int main(void)
{
    PlayerOpts  o;
    char        args[128];
    const char *p;
    uint8_t     debug = FALSE, quit = FALSE, noSound = FALSE;
    int         len;

    memset(&o, 0, sizeof(o));
    o.latencyMs = 0;                                        // nothing to buffer - the newest frame, now
    o.palDelay  = 1;
    o.flags     = MED_OPF_LIVE | MED_OPF_GAME;

    len = (uint8_t) _base->p_cmdlin[0];
    if(len > 126) {
        len = 0;
    }
    memcpy(args, _base->p_cmdlin + 1, len);
    args[len] = 0;
    for(p = args; *p; p++) {
        if(*p == '-' && (p[1] | 0x20) == 'd') {
            debug = TRUE;
        }
        if(*p == '-' && (p[1] | 0x20) == 'n') {
            noSound = TRUE;
        }
        if(*p == '-' && (p[1] | 0x20) == 'h') {
            o.flags |= MED_OPF_HALF_RATE;
        }
        if(*p == '-' && (p[1] | 0x20) == 'f') {
            const char *q = p + 2;
            o.maxFps = 0;
            while(*q >= '0' && *q <= '9') {
                o.maxFps = o.maxFps * 10 + (*q++ - '0');
            }
        }
    }

    print("\33E");
    print("Cosmo2ex Doom - runs on the host\r\n\r\n");

    dbgStart(debug ? 'C' : 0);
    dbgLog("CE_DOOM build");
    dbgLog(buildStamp);

    if(!playerDetect(&o, TRUE, noSound)) {                  // a screen, and DMA or YM sound
        return fail(playerError);
    }
    if(!playerOpen(&o, "doom", progress)) {
        return fail(playerError);
    }
    if(!playerStart()) {
        playerStop();
        return fail(playerError);
    }
    mouseReports(FALSE);
    Supexec(keysOnSuper);
    dbgLog("playing");

    while(!quit) {
        uint8_t codes[6], n = 0;

        while(keyTail != keyHead && n < sizeof(codes)) {   // keys first - they matter most
            uint8_t c = keyRing[keyTail];
            keyTail = (keyTail + 1) & 63;
            if(c == KEY_UNDO) {
                quit = TRUE;
            }
            codes[n++] = c;
        }
        if(n > 0 && !quit) {
            medInput(codes, n);
        }

        if(!playerPoll()) {                                 // the game ended (quit from its menu)
            break;
        }
    }

    Supexec(keysOffSuper);
    playerStop();                                           // CLOSE stops Doom on the host
    mouseReports(TRUE);
    (void) Kbshift(0);                                      // TOS missed the releases - no Shift / Alt left held
    while(Bconstat(2)) {
        Bconin(2);
    }

    print("\33E");
    if(playerError[0]) {
        return fail(playerError);
    }
    dbgLog("end");
    return 0;
}
