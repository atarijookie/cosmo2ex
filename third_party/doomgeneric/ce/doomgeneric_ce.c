// doomgeneric port for CosmosEx: Doom runs as a child process of ce_media, which shows it on the Atari ST.
//
//   stdout: records, each starting with its type byte:
//             'F' + 320 x 200 RGB (3 bytes per pixel)                  a frame
//             'S' + channel, volume (0..127), separation (0..254), name length, lump name (e.g. "dspistol")
//                                                                      a sound effect starts on a channel
//             'U' + channel, volume, separation                        its volume / position changed
//             'X' + channel                                            it stopped
//           ce_media mixes the sound effects itself (it reads them from the same WAD) - at the Atari's rate,
//           in step with the frames, so there's no second clock to drift
//   stdin:  keys   - 2 bytes each: 1 = pressed / 0 = released, Doom key code (doomkeys.h); stdin closed = quit
//
// No music yet (it would need an OPL / MIDI synth). Doom itself prints messages with printf - they go to stderr,
// the records have stdout to themselves.
//
// This file is part of the CosmosEx build of doomgeneric and, like doomgeneric, under the GNU GPL version 2
// (see ../LICENSE). It's a separate program: ce_media only starts it and talks to it through the pipes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"
#include "m_misc.h"

#if DOOMGENERIC_RESX != 320 || DOOMGENERIC_RESY != 200
#error "build with -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DCMAP256"
#endif

#define KEYQUEUE_SIZE   64
#define FRAME_MS        28                          // at most ~35 frames per second

#define CHANNELS        16

static int      frameFd = -1;
static uint8_t  rgb[1 + DOOMGENERIC_RESX * DOOMGENERIC_RESY * 3];      // 'F' + the frame
static uint8_t  inBuf[2];
static int      inHave;
static uint16_t keyQueue[KEYQUEUE_SIZE];
static unsigned keyRead, keyWrite;
static struct timespec t0;

static void writeAll(int fd, const uint8_t *p, size_t n)
{
    while(n > 0) {
        ssize_t w = write(fd, p, n);
        if(w < 0) {
            if(errno == EINTR) {
                continue;
            }
            exit(0);                                // ce_media went away
        }
        p += w;
        n -= (size_t) w;
    }
}

// keys from stdin - non-blocking, so the game runs on without them
static void readKeys(void)
{
    for(;;) {
        ssize_t r = read(0, inBuf + inHave, 2 - inHave);
        if(r == 0) {
            exit(0);                                // stdin closed: ce_media stopped the game
        }
        if(r < 0) {
            return;                                 // EAGAIN - nothing now
        }
        inHave += (int) r;
        if(inHave == 2) {
            if(keyWrite - keyRead < KEYQUEUE_SIZE) {
                keyQueue[keyWrite++ % KEYQUEUE_SIZE] = (uint16_t) ((inBuf[0] ? 0x100 : 0) | inBuf[1]);
            }
            inHave = 0;
        }
    }
}

void DG_Init(void)
{
}

void DG_DrawFrame(void)
{
    const uint8_t *src = (const uint8_t *) DG_ScreenBuffer;     // CMAP256: palette indexes
    uint8_t *d = rgb + 1;
    int i;

    for(i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
        struct color c = colors[src[i]];
        *d++ = c.r;
        *d++ = c.g;
        *d++ = c.b;
    }
    rgb[0] = 'F';
    writeAll(frameFd, rgb, sizeof(rgb));
    readKeys();

    // Doom draws as often as it can, but its world moves 35 times a second - more frames only cost the host's CPU
    {
        static uint32_t last;
        uint32_t now = DG_GetTicksMs();
        if(now - last < FRAME_MS) {
            DG_SleepMs(FRAME_MS - (now - last));
        }
        last = DG_GetTicksMs();
    }
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while(nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t) ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    readKeys();
    if(keyRead == keyWrite) {
        return 0;
    }
    uint16_t k = keyQueue[keyRead++ % KEYQUEUE_SIZE];
    *pressed = (k & 0x100) ? 1 : 0;
    *key     = (unsigned char) k;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void) title;
}

//--------------------------------------------------------------------------------------------------------------------
// sound effects: only reported - ce_media plays them (see the top). Music: none yet.

int   use_libsamplerate   = 0;                      // config variables of i_sound.c (SDL's resampler there)
float libsamplerate_scale = 0.65f;

static boolean  sfxPrefix;
static uint32_t chanEnd[CHANNELS];                  // when the sound on the channel ends (DG_GetTicksMs)

static snddevice_t soundDevices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                      SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

static void sfxLumpName(sfxinfo_t *sfx, char *buf, size_t len)
{
    if(sfx->link != NULL) {
        sfx = sfx->link;
    }
    if(sfxPrefix) {
        M_snprintf(buf, len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), len);
    }
}

static boolean sndInit(boolean useSfxPrefix)
{
    sfxPrefix = useSfxPrefix;
    return true;
}

static void sndShutdown(void)
{
}

static int sndGetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];
    sfxLumpName(sfx, name, sizeof(name));
    return W_GetNumForName(name);
}

static void sndUpdate(void)
{
}

static void sndUpdateParams(int ch, int vol, int sep)
{
    if(ch < 0 || ch >= CHANNELS) {
        return;
    }
    uint8_t r[4] = { 'U', (uint8_t) ch, (uint8_t) vol, (uint8_t) sep };
    writeAll(frameFd, r, sizeof(r));
}

static int sndStart(sfxinfo_t *sfx, int ch, int vol, int sep)
{
    char name[9];
    uint8_t r[4 + 1 + 8];
    int lump, len;

    if(ch < 0 || ch >= CHANNELS) {
        return -1;
    }
    sfxLumpName(sfx, name, sizeof(name));
    lump = W_CheckNumForName(name);
    if(lump < 0) {
        return -1;
    }

    // DMX sound lump: format 3 (W), sample rate (W), samples (DW) - how long it plays, for sndIsPlaying()
    len = W_LumpLength(lump);
    uint32_t ms = 0;
    if(len >= 8) {
        const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);
        uint32_t rate    = d[2] | (d[3] << 8);
        uint32_t samples = d[4] | (d[5] << 8) | ((uint32_t) d[6] << 16) | ((uint32_t) d[7] << 24);
        if(d[0] == 3 && rate > 0) {
            ms = (uint32_t) ((uint64_t) samples * 1000 / rate);
        }
        W_ReleaseLumpNum(lump);
    }
    chanEnd[ch] = DG_GetTicksMs() + ms;

    size_t n = strlen(name);
    r[0] = 'S';
    r[1] = (uint8_t) ch;
    r[2] = (uint8_t) vol;
    r[3] = (uint8_t) sep;
    r[4] = (uint8_t) n;
    memcpy(r + 5, name, n);
    writeAll(frameFd, r, 5 + n);
    return ch;
}

static void sndStop(int ch)
{
    if(ch < 0 || ch >= CHANNELS) {
        return;
    }
    chanEnd[ch] = 0;
    uint8_t r[2] = { 'X', (uint8_t) ch };
    writeAll(frameFd, r, sizeof(r));
}

static boolean sndIsPlaying(int ch)
{
    return ch >= 0 && ch < CHANNELS && (int32_t) (chanEnd[ch] - DG_GetTicksMs()) > 0;
}

static void sndCache(sfxinfo_t *sounds, int num)
{
    (void) sounds;
    (void) num;
}

sound_module_t DG_sound_module = {
    soundDevices, arrlen(soundDevices),
    sndInit, sndShutdown, sndGetSfxLumpNum, sndUpdate, sndUpdateParams, sndStart, sndStop, sndIsPlaying, sndCache
};

// music: not played (yet)
static boolean musInit(void)                            { return true; }
static void    musShutdown(void)                        { }
static void    musSetVolume(int v)                      { (void) v; }
static void    musPause(void)                           { }
static void    musResume(void)                          { }
static void   *musRegister(void *data, int len)         { (void) data; (void) len; return NULL; }
static void    musUnregister(void *h)                   { (void) h; }
static void    musPlay(void *h, boolean looping)        { (void) h; (void) looping; }
static void    musStop(void)                            { }
static boolean musIsPlaying(void)                       { return false; }
static void    musPoll(void)                            { }

static snddevice_t musicDevices[] = { SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
                                      SNDDEVICE_GENMIDI, SNDDEVICE_AWE32 };

music_module_t DG_music_module = {
    musicDevices, arrlen(musicDevices),
    musInit, musShutdown, musSetVolume, musPause, musResume, musRegister, musUnregister, musPlay, musStop,
    musIsPlaying, musPoll
};

int main(int argc, char **argv)
{
    clock_gettime(CLOCK_MONOTONIC, &t0);

    frameFd = dup(1);                               // the frames keep the real stdout...
    if(frameFd < 0 || dup2(2, 1) < 0) {             // ...and Doom's printf goes to stderr
        return 1;
    }
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    doomgeneric_Create(argc, argv);
    for(;;) {
        doomgeneric_Tick();
    }
    return 0;
}
