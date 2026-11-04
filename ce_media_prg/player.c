// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <mint/falcon.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "../libacsiscsi/hdd_if.h"
#include "med.h"
#include "player.h"
#include "dbg.h"

// Screen address and palette: the VBL routine writes the video base registers, which the shifter takes at the start
// of the NEXT frame, while palette registers act at once. So the palette of a new frame is written 'palDelay' VBLs
// (default 1) after its address - else one frame would show the new palette on the old picture. (-p0 on the
// command line if the hardware turns out to take the address right away.)

#define FQ_SIZE         256             // frame queue - power of 2, more than MAX_CHUNKS * MED_CHUNK_MAX_FRAMES
#define FQ_MASK         (FQ_SIZE - 1)
#define MAX_CHUNKS      32
#define RESERVE_BYTES   (48 * 1024L)    // left for TOS and others
#define TT_SCREEN_BYTES 153600L         // TT mid 640x480x4 and TT high 1280x960x1

#define HZ200           (*((volatile uint32_t *) 0x4ba))
#define CONTERM         (*((volatile uint8_t *)  0x484))
#define NVBLS           (*((volatile int16_t *)  0x454))
#define VBLQUEUE        (*((void (***)(void)) 0x456))

#define VID_BASE_H      (*((volatile uint8_t *) 0xffff8201))
#define VID_BASE_M      (*((volatile uint8_t *) 0xffff8203))
#define ST_PALETTE      ((volatile uint16_t *) 0xffff8240)
#define TT_PALETTE      ((volatile uint16_t *) 0xffff8400)
#define TT_SHIFT_MODE   (*((volatile uint16_t *) 0xffff8262))

#define DMA_CTRL        (*((volatile uint8_t *) 0xffff8901))
#define DMA_START_H     (*((volatile uint8_t *) 0xffff8903))
#define DMA_START_M     (*((volatile uint8_t *) 0xffff8905))
#define DMA_START_L     (*((volatile uint8_t *) 0xffff8907))
#define DMA_CNT_H       (*((volatile uint8_t *) 0xffff8909))
#define DMA_CNT_M       (*((volatile uint8_t *) 0xffff890b))
#define DMA_CNT_L       (*((volatile uint8_t *) 0xffff890d))
#define DMA_END_H       (*((volatile uint8_t *) 0xffff890f))
#define DMA_END_M       (*((volatile uint8_t *) 0xffff8911))
#define DMA_END_L       (*((volatile uint8_t *) 0xffff8913))
#define DMA_MODE        (*((volatile uint8_t *) 0xffff8921))

#define YM_SELECT       (*((volatile uint8_t *) 0xffff8800))
#define YM_WRITE        (*((volatile uint8_t *) 0xffff8802))
#define MFP_TACR        (*((volatile uint8_t *) 0xfffffa19))
#define MFP_TADR        (*((volatile uint8_t *) 0xfffffa1f))
#define VBL_VECTOR      (*((volatile uint32_t *) 0x70))  // level 4 autovector
#define ST_SHIFT_MODE   (*((volatile uint8_t *)  0xffff8260))
#define TIMER_A_VECTOR  (*((volatile uint32_t *) 0x134))

PlayerStats playerStats;
char        playerTitle[128];
char        playerError[128];

static PlayerOpts opts;

// stream, from GET_INFO
static uint8_t  vmode, afmt, framesPerChunk, planes, chunkSectors, dmaMode, ymCtrl, ymData;
static uint16_t frameW, frameH;
static uint32_t frameBytes, slotBytes, chunkBytes, audioMax, rate, fpsX1000, durationMs;
static uint32_t latencyTicks, frameTicks, lowWater;

static uint32_t gen, genStartMs;

// memory
static uint8_t *mem;
static uint8_t *chunkBuf[MAX_CHUNKS];
static uint32_t chunkEnd[MAX_CHUNKS];   // frame serial after its last frame
static uint8_t  chunkBusy[MAX_CHUNKS];  // has frames not yet done
static uint16_t nChunks, chunkNext;
static uint8_t *ring;
static uint32_t ringSize;
static uint8_t *screens[2];             // TT: the frame is copied to the back one, then they swap; ST: [0] = black
static uint8_t  backScreen;
static uint8_t  copyFrames;             // TT - frames are copied to the screen (window), not shown from the chunk
static uint32_t screenRowBytes, frameRowBytes, windowOffset;
static uint8_t  ymSilence;
static uint8_t  dmaHiMask = 0x3f;       // STE: 22 bit addresses, TT / Falcon: 24 bit (more ST RAM)

// with the VBL routine and the timer A interrupt (ymisr.s)
volatile uint32_t audPlayed;            // clock: samples played (ticks of the 200 Hz timer without sound)
volatile uint32_t audWritten;           // samples in the ring buffer up to here
// ymisr.s: the interrupt, its sample pointer (an address inside its code), the table next to it, the ring wrap
extern uint8_t * volatile ymIsrPtr;
extern uint8_t  ymTable[MED_YM_TABLE_SIZE];
extern uint32_t ymRingEnd, ymRingBytes;
void     ymIsr(void);
void     ymWrap(void);
#define YM_SPILL        1024            // samples after the ring, a copy of its start - the VBL wraps the pointer back
static volatile uint32_t lastYmPos;

typedef struct {
    uint32_t tick;
    uint8_t *data;
    uint8_t *pal;                       // 16 palette words
} FrameEnt;

static FrameEnt          fq[FQ_SIZE];
static volatile uint32_t fqWrite;       // frames put in the queue (serials)
static volatile uint32_t fqShown;       // serial + 1 of the frame on the screen, 0 = none
static volatile uint8_t  vblActive, running, buffering, paused, endReached, finished;
static volatile uint32_t lastDmaPos, lastHz;
static uint8_t * volatile palPending;
static uint8_t           palCopy[MED_CHUNK_PALETTE_SIZE];   // the chunk may be reused before the palette is written
static volatile uint8_t  palCountdown;
static uint16_t          ttPalBank;

// saved state
static uint8_t  videoTaken, soundTaken, vblInstalled, flockTaken;
static int16_t  oldRez, oldShift;
static void    *oldPhys, *oldLog;
static uint16_t oldPal[16];
static uint16_t oldTTPal[256];
static uint8_t  oldConterm, oldMixer;
static uint32_t oldTimerA;
void            vblHandler(void);       // vbl.s - instead of TOS's VBL handler while the DMA sound is ours
static uint32_t oldVblVector;
static uint8_t  oldStShift;
static uint16_t oldTTShift;
static int16_t  vblSlot = -1;

//--------------------------------------------------------------------------------------------------------------------
// detection

uint8_t playerDetect(PlayerOpts *o, uint8_t wantVideo, uint8_t noSound)
{
    uint32_t mch = readCookie(0x5f4d4348UL) >> 16;          // '_MCH': 0 ST, 1 STE, 2 TT, 3 Falcon
    uint32_t snd = readCookie(0x5f534e44UL);                // '_SND': bit 1 = 8 bit DMA sound
    int16_t  rez = Getrez();

    switch(mch) {
        case 1:  o->machine = MED_MACH_STE;    break;
        case 2:  o->machine = MED_MACH_TT;     break;
        case 3:  o->machine = MED_MACH_FALCON; break;
        default: o->machine = MED_MACH_ST;     break;
    }

    o->aout = noSound ? MED_AOUT_NONE : ((snd & 2) ? MED_AOUT_DMA : MED_AOUT_YM);

    o->vmode = MED_VMODE_NONE;
    if(wantVideo) {
        switch(o->machine) {
            case MED_MACH_TT:                               // full screen ST modes: 32 kB frames (20 fps over ACSI),
                switch(rez) {                               // shown straight from the chunk, no CPU work. TT low / mid
                    case 6:  o->vmode = MED_VMODE_TT_HIGH_WIN;  break;  // would need 77..154 kB frames for full screen.
                    case 2:  o->vmode = MED_VMODE_ST_HIGH;      break;  // TT high monitor shows only TT high - a window
                    default: o->vmode = MED_VMODE_ST_LOW;       break;  // ST low / mid, TT low / mid (VGA)
                }
                break;
            case MED_MACH_FALCON:                           // only its ST compatible modes, no switching
                if(rez == 0) {
                    o->vmode = MED_VMODE_ST_LOW;
                } else if(rez == 2) {
                    o->vmode = MED_VMODE_ST_HIGH;
                } else {
                    strcpy(playerError, "Falcon: switch to ST low or ST high first");
                    return FALSE;
                }
                break;
            default:                                        // ST / STE: mono monitor = ST high, else ST low
                o->vmode = (rez == 2) ? MED_VMODE_ST_HIGH : MED_VMODE_ST_LOW;
                break;
        }
    }
    return TRUE;
}

//--------------------------------------------------------------------------------------------------------------------
// open

static void waitVbls(int n)
{
    while(n-- > 0) {
        Vsync();
    }
}

static uint8_t waitReady(void (*progress)(const char *msg));

uint8_t playerOpen(PlayerOpts *o, const char *atariPath, void (*progress)(const char *msg))
{
    uint8_t hdr[MED_OPEN_PATH];
    uint8_t st;

    opts = *o;
    if(!medInit(o->machine)) {
        strcpy(playerError, "Not enough memory");
        return FALSE;
    }
    dbgLogNum("machine / vmode / aout", (o->machine << 16) | (o->vmode << 8) | o->aout);
    dbgLogNum("medBuf", (uint32_t) medBuf);
    if(!medFind(o->machine)) {
        strcpy(playerError, "Cosmo2ex with the media module not found");
        return FALSE;
    }

    memset(hdr, 0, sizeof(hdr));
    hdr[MED_OPEN_MACHINE]     = o->machine;
    hdr[MED_OPEN_VMODE]       = o->vmode;
    hdr[MED_OPEN_AOUT]        = o->aout;
    putWordBE(hdr + MED_OPEN_LATENCY, o->latencyMs);
    hdr[MED_OPEN_MAX_FPS]     = o->maxFps;
    hdr[MED_OPEN_FLAGS]       = o->flags;
    hdr[MED_OPEN_MAX_SECTORS] = MAXSECTORS;
    putDwordBE(hdr + MED_OPEN_START_MS, o->startMs);

    dbgLog("device found, OPEN");
    st = medOpen(hdr, atariPath);
    dbgLogNum("OPEN status", st);
    switch(st) {
        case MED_OK:                break;
        case MED_ERR_NOT_FOUND:     strcpy(playerError, "File not found, or not on a translated drive"); return FALSE;
        case MED_ERR_NOT_RUNNING:   strcpy(playerError, "The host can't start ce_media");               return FALSE;
        default:                    strcpy(playerError, "OPEN failed");                                return FALSE;
    }

    if(!waitReady(progress)) {                              // CLOSE - else the host keeps decoding / the game running
        medClose();
        return FALSE;
    }
    return TRUE;
}

// after a successful OPEN: wait for READY, take the stream's parameters (and the YM table)
static uint8_t waitReady(void (*progress)(const char *msg))
{
    static char lastMsg[MED_INFO_MSG_SIZE];
    int i;

    // The host probes the file and starts the decoder - or first downloads the ffmpeg docker image, which can take
    // many minutes. No timeout: it says what it's doing (or that it failed); Esc gives up.
    lastMsg[0] = 0;
    for(;;) {
        if(medGetInfo() != MED_OK) {
            strcpy(playerError, "GET_INFO failed");
            return FALSE;
        }
        medBuf[MED_INFO_MSG + MED_INFO_MSG_SIZE - 1] = 0;
        if(medBuf[MED_INFO_STATE] == MED_STATE_READY) {
            break;
        }
        switch(medBuf[MED_INFO_STATE]) {
            case MED_STATE_STARTING:
                if(progress && strcmp(lastMsg, (char *) medBuf + MED_INFO_MSG) != 0) {
                    strcpy(lastMsg, (char *) medBuf + MED_INFO_MSG);
                    progress(lastMsg);
                }
                break;
            case MED_STATE_ERROR:
                strncpy(playerError, (char *) medBuf + MED_INFO_MSG, sizeof(playerError) - 1);
                return FALSE;
            default:
                strcpy(playerError, "The host closed the stream");
                return FALSE;
        }
        while(Bconstat(2)) {
            if(((Bconin(2) >> 16) & 0xff) == 0x01) {        // Esc
                medClose();
                strcpy(playerError, "Cancelled");
                return FALSE;
            }
        }
        waitVbls(10);
    }

    vmode          = medBuf[MED_INFO_VMODE];
    afmt           = medBuf[MED_INFO_AFMT];
    framesPerChunk = medBuf[MED_INFO_FRAMES_PER_CHUNK];
    frameBytes     = getWordBE(medBuf + MED_INFO_FRAME_BYTES);
    slotBytes      = (uint32_t) getWordBE(medBuf + MED_INFO_SLOT_SECTORS) * 512;
    rate           = getDwordBE(medBuf + MED_INFO_CLOCK_RATE);
    dmaMode        = medBuf[MED_INFO_DMA_MODE];
    ymCtrl         = medBuf[MED_INFO_YM_TIMER_CTRL];
    ymData         = medBuf[MED_INFO_YM_TIMER_DATA];
    fpsX1000       = getDwordBE(medBuf + MED_INFO_FPS_X1000);
    durationMs     = getDwordBE(medBuf + MED_INFO_DURATION_MS);
    chunkSectors   = (uint8_t) getWordBE(medBuf + MED_INFO_CHUNK_SECTORS);
    audioMax       = getWordBE(medBuf + MED_INFO_AUDIO_MAX);
    frameW         = getWordBE(medBuf + MED_INFO_FRAME_W);
    frameH         = getWordBE(medBuf + MED_INFO_FRAME_H);
    planes         = medBuf[MED_INFO_PLANES];
    chunkBytes     = (uint32_t) chunkSectors * 512;
    strncpy(playerTitle, (char *) medBuf + MED_INFO_TITLE, sizeof(playerTitle) - 1);

    if(rate == 0 || chunkSectors == 0 || framesPerChunk > MED_CHUNK_MAX_FRAMES || (vmode != MED_VMODE_NONE && fpsX1000 == 0) ||
       (vmode != MED_VMODE_NONE && vmode != opts.vmode)) {
        strcpy(playerError, "The host sent a stream this player doesn't know");
        return FALSE;
    }

    if(afmt == MED_AFMT_YM) {
        uint16_t best = 0xffff;
        if(medGetYmTable() != MED_OK) {
            strcpy(playerError, "GET_YM_TABLE failed");
            return FALSE;
        }
        memcpy(ymTable, medBuf, MED_YM_TABLE_SIZE);
        for(i=0; i<256; i++) {                              // silence = the entry nearest to the middle level
            uint16_t level = (uint16_t) getDwordBE(ymTable + i * MED_YM_TABLE_ENTRY + 12);
            uint16_t d = (level > 32768) ? level - 32768 : 32768 - level;
            if(d < best) {
                best = d;
                ymSilence = (uint8_t) i;
            }
        }
    }

    gen = 0;                                                // taken from the first chunk - see fetch()
    return TRUE;
}

uint8_t playerHasVideo(void)
{
    return vmode != MED_VMODE_NONE;
}

//--------------------------------------------------------------------------------------------------------------------
// hardware - all of it in supervisor mode (Supexec, or the VBL / timer interrupts)

static uint32_t dmaCounter(void)
{
    uint32_t a, b;
    do {
        a = ((uint32_t) (DMA_CNT_H & dmaHiMask) << 16) | ((uint32_t) DMA_CNT_M << 8) | DMA_CNT_L;
        b = ((uint32_t) (DMA_CNT_H & dmaHiMask) << 16) | ((uint32_t) DMA_CNT_M << 8) | DMA_CNT_L;
    } while(a != b);
    return a;
}

static void dmaSetStart(uint32_t a)
{
    DMA_START_H = a >> 16;
    DMA_START_M = a >> 8;
    DMA_START_L = a;
}

static void dmaSetEnd(uint32_t a)
{
    DMA_END_H = a >> 16;
    DMA_END_M = a >> 8;
    DMA_END_L = a;
}

static void soundOff(void)
{
    switch(afmt) {
        case MED_AFMT_DMA_S8_MONO:  DMA_CTRL = 0;   break;
        case MED_AFMT_YM:   MFP_TACR = 0;   break;
    }
}

// starts the sound where the clock is
static void soundOn(void)
{
    uint32_t pos = audPlayed % ringSize;
    uint32_t base = (uint32_t) ring;
    int i;

    switch(afmt) {
        case MED_AFMT_DMA_S8_MONO:
            pos &= ~1UL;
            DMA_CTRL = 0;
            dmaSetStart(base + pos);
            dmaSetEnd(base + ringSize);
            DMA_MODE = dmaMode;
            DMA_CTRL = 3;                                   // play, loop
            for(i=0; i<20000; i++) {                        // the start was taken when the counter is in there...
                uint32_t c = dmaCounter();
                if(c >= base + pos && c < base + pos + 512) {
                    break;
                }
            }
            dmaSetStart(base);                              // ...so the loops go over the whole ring
            lastDmaPos = pos;
            break;

        case MED_AFMT_YM:
            ymIsrPtr  = ring + pos;
            lastYmPos = pos;
            MFP_TADR = ymData;
            MFP_TACR = ymCtrl;
            break;

        default:
            lastHz = HZ200;
            break;
    }
}

static void soundOnSuper(void)  { soundOn(); }
static void soundOffSuper(void) { soundOff(); }

// clock: samples played since the last call
static void clockTick(void)
{
    if(!running) {
        return;
    }
    switch(afmt) {
        case MED_AFMT_DMA_S8_MONO: {
            uint32_t base = (uint32_t) ring;
            uint32_t c = dmaCounter();
            if(c >= base && c <= base + ringSize) {
                uint32_t pos = c - base;
                if(pos == ringSize) {
                    pos = 0;
                }
                audPlayed += (pos >= lastDmaPos) ? pos - lastDmaPos : pos + ringSize - lastDmaPos;
                lastDmaPos = pos;
            }
            break;
        }
        case MED_AFMT_YM: {                                 // from the interrupt's pointer, like the DMA counter
            uint32_t pos = (uint32_t) (ymIsrPtr - ring);
            if(pos >= ringSize) {                           // in the spill area
                pos -= ringSize;
            }
            audPlayed += (pos >= lastYmPos) ? pos - lastYmPos : pos + ringSize - lastYmPos;
            lastYmPos = pos;
            ymWrap();
            break;
        }
        default: {
            uint32_t now = HZ200;
            audPlayed += now - lastHz;
            lastHz = now;
            break;
        }
    }
}

static void setBase(const uint8_t *a)
{
    VID_BASE_H = (uint32_t) a >> 16;                        // always 256 B aligned - the low byte stays 0
    VID_BASE_M = (uint32_t) a >> 8;
}

static void writePalette(const uint8_t *pal)
{
    volatile uint16_t *regs = (vmode == MED_VMODE_TT_MID_WIN) ? TT_PALETTE + ttPalBank * 16 : ST_PALETTE;
    int i;
    for(i=0; i<16; i++) {
        regs[i] = ((uint16_t) pal[i * 2] << 8) | pal[i * 2 + 1];
    }
}

static void copyWindow(uint8_t *screen, const uint8_t *frame)
{
    uint32_t *d = (uint32_t *) (screen + windowOffset);
    const uint32_t *s = (const uint32_t *) frame;
    uint16_t longs = frameRowBytes / 4;
    uint16_t skip  = (screenRowBytes - frameRowBytes) / 4;
    uint16_t y, x;

    for(y=0; y<frameH; y++) {
        for(x=0; x<longs; x++) {
            *d++ = *s++;
        }
        d += skip;
    }
}

static void present(const FrameEnt *f)
{
    if(copyFrames) {
        uint8_t *scr = screens[backScreen];
        copyWindow(scr, f->data);
        setBase(scr);
        backScreen ^= 1;
    } else {
        setBase(f->data);
    }

    if(planes > 1) {
        if(opts.palDelay == 0) {
            writePalette(f->pal);
        } else {
            memcpy(palCopy, f->pal, sizeof(palCopy));
            palPending   = palCopy;
            palCountdown = opts.palDelay;
        }
    }
}

// in the VBL queue - runs 50 / 60 / 71 times a second, also while the main loop waits for a transfer
static void vblRoutine(void)
{
    if(!vblActive) {
        return;
    }

    if(palPending) {
        if(--palCountdown == 0) {
            writePalette(palPending);
            palPending = NULL;
        }
    }

    clockTick();

    if(running) {
        int32_t ahead = (int32_t) (audWritten - audPlayed);
        if(afmt != MED_AFMT_NONE) {
            if(endReached && ahead <= 0) {                  // all played
                soundOff();
                running = 0;
                finished = 1;
            } else if(!endReached && ahead < (int32_t) lowWater) {   // out of data - stop before it plays old data
                soundOff();
                running = 0;
                buffering = 1;
                playerStats.underruns++;
            }
        } else if(opts.flags & MED_OPF_LIVE) {              // live: no clock to keep - it ends when the game does
            if(endReached && fqShown >= fqWrite) {
                running = 0;
                finished = 1;
            }
        } else if(fqShown >= fqWrite) {                     // no sound: the frames are all there is
            uint32_t lastTick = fqWrite ? fq[(fqWrite - 1) & FQ_MASK].tick : 0;
            if(audPlayed >= lastTick + frameTicks) {
                running = 0;
                if(endReached) {
                    finished = 1;
                } else {
                    buffering = 1;
                    playerStats.underruns++;
                }
            }
        }
    }

    if(vmode != MED_VMODE_NONE && fqShown < fqWrite) {
        uint32_t n = fqShown;
        if(opts.flags & MED_OPF_LIVE) {                     // live (a game): the newest frame, now - its time
            playerStats.skipped += fqWrite - 1 - n;         // stamp is when the host made it, not when to show it
            n = fqWrite - 1;
            present(&fq[n & FQ_MASK]);
            fqShown = n + 1;
            playerStats.frames++;
        } else if(fq[n & FQ_MASK].tick <= audPlayed) {
            while(n + 1 < fqWrite && fq[(n + 1) & FQ_MASK].tick <= audPlayed) {    // late - show the newest one due
                n++;
                playerStats.skipped++;
            }
            present(&fq[n & FQ_MASK]);
            fqShown = n + 1;
            playerStats.frames++;
        }
    }
}

static void installVblSuper(void)
{
    void (**q)(void) = VBLQUEUE;
    int16_t i;
    for(i=1; i<NVBLS; i++) {                                // 0 is the floppy's
        if(q[i] == NULL) {
            q[i] = vblRoutine;
            vblSlot = i;
            return;
        }
    }
}

static void removeVblSuper(void)
{
    if(vblSlot > 0) {
        VBLQUEUE[vblSlot] = NULL;
        vblSlot = -1;
    }
}

static void takeSoundSuper(void)
{
    if(afmt == MED_AFMT_DMA_S8_MONO) {
        DMA_CTRL = 0;
        // the sound must not look like a monitor change to TOS's VBL handler - see vbl.s
        oldStShift = ST_SHIFT_MODE;
        if(opts.machine == MED_MACH_TT) {
            oldTTShift = TT_SHIFT_MODE;
        }
        if(opts.machine == MED_MACH_STE || opts.machine == MED_MACH_TT) {
            oldVblVector = VBL_VECTOR;
            VBL_VECTOR   = (uint32_t) vblHandler;
        }
    } else if(afmt == MED_AFMT_YM) {
        int r;
        YM_SELECT = 7;
        oldMixer = YM_SELECT;
        YM_WRITE = oldMixer | 0x3f;                         // tones and noise off - the volume registers are the DAC
        for(r=8; r<=10; r++) {
            YM_SELECT = r;
            YM_WRITE = 0;
        }
        oldTimerA = TIMER_A_VECTOR;
        *((volatile int16_t *) 0x43e) = -1;                 // flock - the floppy VBL leaves the YM alone
    }
    oldConterm = CONTERM;
    CONTERM = oldConterm & ~5;                              // no key click, no bell - they use the YM
}

static void giveSoundSuper(void)
{
    if(afmt == MED_AFMT_DMA_S8_MONO) {
        uint32_t until;
        DMA_CTRL = 0;
        until = HZ200 + 10;                                 // 50 ms: GPIP 7 is back to the monitor's level, and a few
        while((int32_t) (HZ200 - until) < 0) {              // VBLs saw that
        }
        if(oldVblVector) {
            VBL_VECTOR   = oldVblVector;
            oldVblVector = 0;
        }
        if(opts.machine == MED_MACH_TT) {                   // in case the mode was changed anyway
            if(TT_SHIFT_MODE != oldTTShift) {
                TT_SHIFT_MODE = oldTTShift;
            }
        } else if(ST_SHIFT_MODE != oldStShift) {
            ST_SHIFT_MODE = oldStShift;
        }
    } else if(afmt == MED_AFMT_YM) {
        int r;
        MFP_TACR = 0;
        for(r=8; r<=10; r++) {
            YM_SELECT = r;
            YM_WRITE = 0;
        }
        YM_SELECT = 7;
        YM_WRITE = oldMixer;
        *((volatile int16_t *) 0x43e) = 0;
    }
    CONTERM = oldConterm;
}

static void restoreTimerASuper(void)
{
    TIMER_A_VECTOR = oldTimerA;
}

static void blackPaletteSuper(void)
{
    int i;
    if(vmode == MED_VMODE_TT_MID_WIN) {
        ttPalBank = TT_SHIFT_MODE & 0x0f;
        for(i=0; i<16; i++) {
            TT_PALETTE[ttPalBank * 16 + i] = 0;
        }
    } else if(planes > 1) {
        for(i=0; i<16; i++) {
            ST_PALETTE[i] = 0;
        }
    }
}

static const uint8_t *blackFrame;
static void showBlackSuper(void)
{
    palPending = NULL;
    if(!copyFrames) {
        setBase(blackFrame);                                // the frame on screen lives in a chunk buffer - not anymore
    }
}

//--------------------------------------------------------------------------------------------------------------------
// start

static void lineaHideMouse(void)
{
    __asm__ volatile (".dc.w 0xa000\n\t.dc.w 0xa00a" : : : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

static void lineaShowMouse(void)
{
    register uint8_t *la __asm__("a0");
    __asm__ volatile (".dc.w 0xa000" : "=r"(la) : : "d0", "d1", "d2", "a1", "a2", "cc", "memory");
    int16_t *intin = *((int16_t **) (la + 8));
    intin[0] = 0;                                           // show no matter how many hides were before
    __asm__ volatile (".dc.w 0xa009" : : : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

static void resetStream(void)
{
    int i;
    vblActive  = 0;
    fqWrite    = 0;
    fqShown    = 0;
    audPlayed  = 0;
    audWritten = 0;
    lastDmaPos = 0;
    endReached = 0;
    finished   = 0;
    running    = 0;
    buffering  = 1;
    palPending = NULL;
    for(i=0; i<MAX_CHUNKS; i++) {
        chunkBusy[i] = 0;
    }
    chunkNext = 0;
    if(ring) {
        memset(ring, (afmt == MED_AFMT_YM) ? ymSilence : 0, ringSize + ((afmt == MED_AFMT_YM) ? YM_SPILL : 0));
        lastYmPos = 0;
    }
    vblActive = 1;
}

uint8_t playerStart(void)
{
    uint8_t  tt     = (opts.machine == MED_MACH_TT || opts.machine == MED_MACH_FALCON);    // ST RAM explicitly
    uint8_t  video  = (vmode != MED_VMODE_NONE);
    uint32_t screenBytes = 0;
    uint32_t chunkTicks  = 0;
    uint32_t avail, need = 0;
    int16_t  k, wantK;
    uint8_t *p;
    int16_t  targetRez = -1;

    latencyTicks = (uint32_t) opts.latencyMs * rate / 1000;
    dmaHiMask    = (opts.machine == MED_MACH_STE) ? 0x3f : 0xff;
    lowWater     = rate / 10;                               // 100 ms: the VBL notices in time, also at 50 Hz
    if(opts.flags & MED_OPF_LIVE) {                         // a game: its sound only a little behind the picture -
        lowWater = rate * 6 / 100;                          // 60 ms is still 3 VBLs to notice, 120 ms buffered
    }
    frameTicks   = video ? rate * 1000 / fpsX1000 : 0;    // all fits in 32 bits: rate <= 50066, <= 8 frames

    if(video) {
        copyFrames = (vmode == MED_VMODE_TT_MID_WIN || vmode == MED_VMODE_TT_HIGH_WIN);
        screenBytes = copyFrames ? 2 * TT_SCREEN_BYTES : 32000;
        chunkTicks  = (uint32_t) framesPerChunk * rate * 1000 / fpsX1000;
        wantK = (int16_t) ((latencyTicks + chunkTicks - 1) / chunkTicks) + 2;   // + the one on screen + the one coming
        if(wantK < 2) {
            wantK = 2;
        }
        if(wantK > MAX_CHUNKS) {
            wantK = MAX_CHUNKS;
        }
    } else {
        wantK = 1;                                          // audio only: the audio is copied out right away
    }

    dbgLogNum("start: afmt / vmode / frames per chunk", (afmt << 16) | (vmode << 8) | framesPerChunk);
    dbgLogNum("rate", rate);
    dbgLogNum("chunk bytes", chunkBytes);
    dbgLogNum("audio max", audioMax);
    avail = (uint32_t) (tt ? Mxalloc(-1, 0) : Malloc(-1));
    dbgLogNum("free (largest block)", avail);
    for(k = wantK; k >= (video ? 2 : 1); k--) {
        ringSize = 0;
        if(afmt != MED_AFMT_NONE) {
            ringSize = video ? (uint32_t) (k + 2) * audioMax : latencyTicks + 3 * audioMax;
            if(opts.flags & MED_OPF_LIVE) {                 // live: no deeper than the latency - a fuller ring
                ringSize = 2 * lowWater + 2 * audioMax;     // would only delay the sound
            } else if(ringSize < 8192) {
                ringSize = 8192;
            }
            ringSize = (ringSize + 511) & ~511UL;
        }
        need = (uint32_t) k * chunkBytes + ringSize + ((afmt == MED_AFMT_YM) ? YM_SPILL : 0) + screenBytes + 1024;
        if(need + RESERVE_BYTES <= avail) {
            break;
        }
    }
    if(k < (video ? 2 : 1)) {
        strcpy(playerError, "Not enough memory");
        return FALSE;
    }
    nChunks = k;
    playerStats.chunkBuffers = k;
    if(video) {
        uint32_t lt = (uint32_t) (k - 2) * chunkTicks;      // what the buffers allow
        if(lt < latencyTicks) {
            latencyTicks = lt;
        }
    } else if(latencyTicks + 2 * audioMax > ringSize) {
        latencyTicks = ringSize - 2 * audioMax;
    }
    if(latencyTicks < lowWater * 2 && afmt != MED_AFMT_NONE) {
        latencyTicks = lowWater * 2;
    }
    playerStats.latencyMs = (uint16_t) (latencyTicks * 1000 / rate);

    dbgLogNum("chunk buffers", k);
    dbgLogNum("ring bytes", ringSize);
    dbgLogNum("allocating", need);
    mem = (uint8_t *) (tt ? Mxalloc(need, 0) : Malloc(need));
    dbgLogNum("memory at", (uint32_t) mem);
    if(!mem || (int32_t) mem < 0) {
        mem = NULL;
        strcpy(playerError, "Not enough memory");
        return FALSE;
    }

    p = (uint8_t *) ((((uint32_t) mem) + 511) & ~511UL);    // chunks first - 512 B aligned keeps every frame 256 B aligned
    for(k=0; k<nChunks; k++) {
        chunkBuf[k] = p;
        p += chunkBytes;
    }
    if(ringSize) {
        ring = p;
        ymRingEnd   = (uint32_t) (ring + ringSize);
        ymRingBytes = ringSize;
        p += ringSize;
        if(afmt == MED_AFMT_YM) {                           // keeps the rest 512 B aligned
            p += YM_SPILL;
        }
    }
    if(video) {                                             // ringSize and chunkBytes are multiples of 512
        screens[0] = p;
        screens[1] = copyFrames ? p + TT_SCREEN_BYTES : p;
        memset(p, (planes == 1) ? 0xff : 0, screenBytes);   // black (mono: pixel 1 = black)
        blackFrame = screens[0];
    }

    resetStream();
    vblActive = 0;

    // screen
    if(video) {
        uint8_t ttMode = (opts.machine == MED_MACH_TT);
        oldRez  = Getrez();
        oldPhys = Physbase();
        oldLog  = Logbase();
        {
            int i;
            for(i=0; i<16; i++) {
                oldPal[i] = Setcolor(i, -1);
            }
        }
        if(ttMode) {
            EgetPalette(0, 256, oldTTPal);
            oldShift = EgetShift();
        }

        lineaHideMouse();
        (void) Cursconf(0, 0);
        Supexec(blackPaletteSuper);

        switch(vmode) {
            case MED_VMODE_ST_LOW:      targetRez = 0;  break;
            case MED_VMODE_ST_HIGH:     targetRez = 2;  break;
            case MED_VMODE_TT_MID_WIN:  targetRez = 4;  break;
            case MED_VMODE_TT_HIGH_WIN: targetRez = 6;  break;
        }
        // Setscreen with the resolution, on the TT too: it sets TOS's own screen state (Line-A, VT52) with the mode -
        // only switching the hardware (EsetShift) left the TT desktop garbled after ST mid -> ST low -> ST mid
        dbgLogNum("video: old rez", oldRez);
        if(ttMode) {
            dbgLogNum("video: old TT shift", (uint16_t) oldShift);
        }
        dbgLogNum("video: Setscreen to rez", (targetRez != oldRez) ? targetRez : 0xffff);
        Setscreen(screens[0], screens[0], (targetRez != oldRez) ? targetRez : -1);
        dbgLogNum("video: Getrez now", Getrez());
        if(ttMode && targetRez != oldRez && Getrez() != targetRez) {    // TOS didn't take that mode - the hardware at least
            EsetShift((oldShift & 0xf8ff) | (targetRez << 8));
        }
        Vsync();
        if(ttMode) {
            dbgLogNum("video: TT shift now", (uint16_t) EgetShift());
        }
        Supexec(blackPaletteSuper);                         // a mode switch may have set the default palette
        videoTaken = 1;

        screenRowBytes = (vmode == MED_VMODE_TT_MID_WIN) ? 320 : (vmode == MED_VMODE_TT_HIGH_WIN) ? 160 : frameW / 16 * planes * 2;
        frameRowBytes  = frameW / 16 * planes * 2;
        if(copyFrames) {                                    // the frame in the middle of the screen
            uint16_t screenH = (vmode == MED_VMODE_TT_MID_WIN) ? 480 : 960;
            windowOffset = (uint32_t) (screenH - frameH) / 2 * screenRowBytes + (screenRowBytes - frameRowBytes) / 2;
        }
        backScreen = 1;
    }

    // sound
    if(afmt != MED_AFMT_NONE) {
        dbgLogNum("ring at", (uint32_t) ring);
        dbgLog("take sound");
        Supexec(takeSoundSuper);
        dbgLog("sound taken");
        if(afmt == MED_AFMT_YM) {
            medKeepFlock = 1;
            hdIf.forceFlock = 1;                            // flock stays ours - ACSI must not wait for it
            flockTaken = 1;
            Xbtimer(0, 0, 0, ymIsr);                        // vector + interrupt on, the timer stays stopped
        }
        soundTaken = 1;
    }

    dbgLog("install VBL routine");
    Supexec(installVblSuper);
    dbgLogNum("VBL slot", (uint32_t) vblSlot);
    if(vblSlot < 0) {
        strcpy(playerError, "No free VBL slot");
        playerStop();
        return FALSE;
    }
    vblInstalled = 1;
    vblActive = 1;
    return TRUE;
}

//--------------------------------------------------------------------------------------------------------------------
// main loop

static uint8_t silence(void)
{
    return (afmt == MED_AFMT_YM) ? ymSilence : 0;
}

static void ringPut(uint32_t pos, uint8_t v)
{
    ring[pos] = v;
    if(afmt == MED_AFMT_YM && pos < YM_SPILL) {
        ring[ringSize + pos] = v;
    }
}

static uint32_t ringFree(void)
{
    return ringSize - (audWritten - audPlayed);
}

static uint8_t canFetch(void)
{
    if(afmt != MED_AFMT_NONE && ringFree() < audioMax + 2) {
        return FALSE;
    }
    if(vmode != MED_VMODE_NONE && chunkBusy[chunkNext]) {
        // free when its frames are done: shown and gone (or copied to the screen)
        uint32_t end = chunkEnd[chunkNext];
        if(copyFrames ? (end > fqShown) : (end >= fqShown)) {
            return FALSE;
        }
        chunkBusy[chunkNext] = 0;
    }
    return TRUE;
}

static void ringWrite(uint32_t start, const uint8_t *src, uint32_t bytes)
{
    uint32_t pos, n;

    if(start < audWritten) {                                // overlaps what's there - skip that part
        uint32_t skip = audWritten - start;
        if(skip >= bytes) {
            return;
        }
        src += skip;
        bytes -= skip;
        start = audWritten;
    }
    while(audWritten < start) {                             // a gap (not expected) - silence, not what was there before
        ringPut(audWritten % ringSize, silence());
        audWritten++;
    }

    pos = start % ringSize;
    while(bytes > 0) {
        n = ringSize - pos;
        if(n > bytes) {
            n = bytes;
        }
        memcpy(ring + pos, src, n);
        if(afmt == MED_AFMT_YM && pos < YM_SPILL) {         // the start of the ring is also in the spill area
            memcpy(ring + ringSize + pos, src, (pos + n > YM_SPILL) ? YM_SPILL - pos : n);
        }
        src += n;
        bytes -= n;
        pos = 0;
        start += n;
    }
    audWritten = start;
}

// clear the ring after the end, so the sound plays silence until the VBL stops it
static void padSilence(void)
{
    uint32_t n = rate / 5, pos = audWritten % ringSize;
    if(n > ringFree()) {
        n = ringFree();
    }
    while(n-- > 0) {
        ringPut(pos, silence());
        if(++pos == ringSize) {
            pos = 0;
        }
    }
}

// one GET_CHUNK; TRUE when a chunk came
static uint8_t fetch(void)
{
    uint8_t *buf = chunkBuf[chunkNext];
    uint8_t  st, flags, n, i;

    st = medGetChunk(buf, chunkSectors, gen);
    if(st != MED_OK) {
        playerStats.errors++;
        return FALSE;
    }

    flags = buf[MED_CHUNK_FLAGS];
    if(gen == 0) {                                          // the 1st generation comes with the 1st chunk
        gen = getDwordBE(buf + MED_CHUNK_GENERATION);
        if(flags & MED_CHF_EMPTY) {
            return FALSE;
        }
    }
    if(getDwordBE(buf + MED_CHUNK_GENERATION) != gen) {     // from before a seek
        return FALSE;
    }
    if(flags & MED_CHF_EMPTY) {
        if(flags & MED_CHF_END) {
            endReached = 1;
            if(ring) {
                padSilence();
            }
        }
        return FALSE;
    }

    playerStats.chunks++;
    if(playerStats.chunks <= 3) {
        dbgLogNum("chunk: bytes of audio", getDwordBE(buf + MED_CHUNK_AUDIO_BYTES));
        dbgLogNum("chunk: audio start", getDwordBE(buf + MED_CHUNK_AUDIO_START));
    }
    genStartMs = getDwordBE(buf + MED_CHUNK_GEN_START_MS);
    n = buf[MED_CHUNK_FRAME_COUNT];
    if(n > framesPerChunk) {
        n = framesPerChunk;
    }

    if(afmt != MED_AFMT_NONE) {
        uint32_t bytes = getDwordBE(buf + MED_CHUNK_AUDIO_BYTES);
        if(bytes > audioMax) {
            bytes = audioMax;
        }
        ringWrite(getDwordBE(buf + MED_CHUNK_AUDIO_START), buf + MED_CHUNK_HEADER_SIZE + n * slotBytes, bytes);
    }

    if(vmode != MED_VMODE_NONE && n > 0) {
        for(i=0; i<n; i++) {
            FrameEnt *f = &fq[fqWrite & FQ_MASK];
            f->tick = getDwordBE(buf + MED_CHUNK_FRAMES + i * MED_CHUNK_FRAME_ENTRY);
            f->data = buf + MED_CHUNK_HEADER_SIZE + i * slotBytes;
            f->pal  = buf + MED_CHUNK_PALETTES + i * MED_CHUNK_PALETTE_SIZE;
            fqWrite++;                                      // after the entry is complete - the VBL reads it
        }
        chunkBusy[chunkNext] = 1;
        chunkEnd[chunkNext]  = fqWrite;
        chunkNext = (chunkNext + 1) % nChunks;
    }

    if(flags & MED_CHF_END) {
        endReached = 1;
        if(ring) {
            padSilence();
        }
    }
    return TRUE;
}

static uint32_t bufferedTicks(void)
{
    if(afmt != MED_AFMT_NONE) {
        return audWritten - audPlayed;
    }
    if(fqWrite > fqShown) {
        uint32_t t = fq[(fqWrite - 1) & FQ_MASK].tick;
        return (t > audPlayed) ? t - audPlayed : 0;
    }
    return 0;
}

uint8_t playerPoll(void)
{
    uint8_t got = FALSE;

    if(finished) {
        return FALSE;
    }
    if(playerStats.errors > 20 && playerStats.chunks == 0) {
        strcpy(playerError, "Transfers from the Cosmo2ex fail");
        return FALSE;
    }

    if(!endReached && canFetch()) {
        got = fetch();
    }

    if(buffering && !paused && !running) {                  // start / go on when there's enough
        if(endReached || bufferedTicks() >= latencyTicks || !canFetch()) {
            buffering = 0;
            if(afmt == MED_AFMT_NONE || audWritten > audPlayed) {
                dbgLogNum("sound on: played", audPlayed);
                dbgLogNum("sound on: written", audWritten);
                Supexec(soundOnSuper);
                dbgLog("sound is on");
                running = 1;
            } else if(endReached) {
                finished = 1;
            }
        }
    }

    if(!got) {
        Vsync();                                            // nothing came - don't hammer the bus
    }
    return TRUE;
}

void playerPause(uint8_t pause)
{
    if(pause && !paused) {
        paused = 1;
        if(running) {
            running = 0;
            Supexec(soundOffSuper);
        }
        buffering = 1;
    } else if(!pause && paused) {
        paused = 0;                                         // playerPoll() starts it when there's enough
    }
}

uint8_t playerPaused(void)
{
    return paused;
}

uint8_t playerBuffering(void)
{
    return buffering && !paused;
}

uint8_t playerAudioFormat(void)
{
    return afmt;
}

uint32_t playerClockRate(void)
{
    return rate;
}

uint32_t playerDurationMs(void)
{
    return durationMs;
}

uint32_t playerPositionMs(void)
{
    uint32_t t = audPlayed;
    return genStartMs + (t / rate) * 1000 + (t % rate) * 1000 / rate;
}

uint8_t playerBufferedPercent(void)
{
    uint32_t b = bufferedTicks();
    if(latencyTicks == 0 || b >= latencyTicks) {
        return 100;
    }
    return (uint8_t) (b * 100 / latencyTicks);
}

void playerSeek(int32_t deltaMs)
{
    int32_t pos = (int32_t) playerPositionMs() + deltaMs;
    if(pos < 0) {
        pos = 0;
    }
    if(durationMs && pos >= (int32_t) durationMs) {
        return;
    }

    running = 0;
    Supexec(soundOffSuper);
    if(vmode != MED_VMODE_NONE) {
        Supexec(showBlackSuper);
    }

    if(medSeek((uint32_t) pos) != MED_OK) {
        playerStats.errors++;
        buffering = 1;                                      // carries on where it was
        return;
    }
    resetStream();
    gen        = getDwordBE(medBuf + 0);
    genStartMs = getDwordBE(medBuf + 4);
}

//--------------------------------------------------------------------------------------------------------------------
// stop

void playerStop(void)
{
    vblActive = 0;
    running = 0;

    dbgLog("stop");
    if(vblInstalled) {
        Supexec(removeVblSuper);
        vblInstalled = 0;
    }
    if(soundTaken) {
        Supexec(giveSoundSuper);
        if(afmt == MED_AFMT_YM) {
            Jdisint(13);                                    // timer A
            Supexec(restoreTimerASuper);
        }
        soundTaken = 0;
    }
    if(flockTaken) {
        medKeepFlock = 0;
        hdIf.forceFlock = 0;
        flockTaken = 0;
    }
    if(videoTaken) {
        dbgLogNum("stop: Getrez", Getrez());
        if(opts.machine == MED_MACH_TT) {
            dbgLogNum("stop: TT shift", (uint16_t) EgetShift());
        }
        dbgLogNum("stop: Setscreen to rez", (Getrez() != oldRez) ? oldRez : 0xffff);
        Setscreen(oldLog, oldPhys, (Getrez() != oldRez) ? oldRez : -1);    // also resets the palette
        dbgLogNum("stop: Getrez now", Getrez());
        if(opts.machine == MED_MACH_TT) {
            dbgLogNum("stop: TT shift now", (uint16_t) EgetShift());
            if(EgetShift() != oldShift) {                   // mode, and the palette bank of TT low / mid
                dbgLog("stop: EsetShift");
                EsetShift(oldShift);
            }
            dbgLog("stop: TT palette");
            EsetPalette(0, 256, oldTTPal);
        }
        dbgLog("stop: ST palette");
        Setpalette(oldPal);
        Vsync();
        dbgLog("stop: cursor, mouse");
        (void) Cursconf(1, 0);
        lineaShowMouse();
        videoTaken = 0;
        dbgLog("stop: video given back");
    }

    dbgLog("stop: CLOSE");
    medClose();

    if(mem) {
        Mfree(mem);
        mem = NULL;
    }
}
