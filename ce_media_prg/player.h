#ifndef _PLAYER_H_
#define _PLAYER_H_

// Playback engine shared by CE_VIDEO.TTP and CE_AUDIO.TTP.
//
// The host sends chunks - one DMA transfer each: a header, up to a few frames already in screen memory format, and
// the audio of those frames. Chunks land in a ring of chunk buffers; ST / STE / Falcon show the frames right from
// there (each is 256 B aligned), TT copies them to the middle of its screen. The audio is copied to a sound ring
// buffer, which the DMA sound plays in a loop (STE / TT / Falcon) or the timer A interrupt plays through the YM
// volume registers (plain ST).
//
// The clock is the count of played samples (200 Hz timer without sound). A VBL routine shows the newest frame whose
// time has come - so the picture changes on time even while the main loop waits for a long ACSI transfer. When the
// sound buffer runs low, playback pauses until 'latency' worth of data is buffered again.

#include <stdint.h>

typedef struct {
    uint8_t  machine;           // MED_MACH_*
    uint8_t  vmode;             // MED_VMODE_* to ask for (NONE: audio only)
    uint8_t  aout;              // MED_AOUT_*
    uint16_t latencyMs;         // buffer before playing
    uint8_t  maxFps;            // 0 = host default
    uint32_t startMs;
    uint8_t  palDelay;          // VBLs between a new screen address and its palette (see player.c)
    uint8_t  flags;             // MED_OPF_*: LIVE - the newest frame is shown as soon as it's here (a game);
                                // GAME - the path is a game on the host ("doom")
} PlayerOpts;

typedef struct {
    uint32_t frames;            // shown
    uint32_t skipped;           // late, not shown
    uint32_t underruns;         // ran out of data, paused to buffer
    uint32_t chunks;
    uint32_t errors;            // failed transfers
    uint16_t chunkBuffers;      // how many fit in memory
    uint16_t latencyMs;         // what they allow
} PlayerStats;

extern PlayerStats playerStats;
extern char        playerTitle[128];
extern char        playerError[128];

// machine, sound, video mode for this machine and monitor; FALSE (playerError) when it can't play
uint8_t  playerDetect(PlayerOpts *o, uint8_t wantVideo, uint8_t noSound);

// find the device, OPEN, wait until the host is READY (shows its messages with 'progress'); FALSE + playerError on error
uint8_t  playerOpen(PlayerOpts *o, const char *atariPath, void (*progress)(const char *msg));

uint8_t  playerHasVideo(void);          // after playerOpen: the stream has frames for the screen

// take memory, screen and sound; FALSE + playerError when there's not enough memory
uint8_t  playerStart(void);

// one round of the main loop: fetch data, start / resume when buffered. FALSE when the stream ended (or fatal error)
uint8_t  playerPoll(void);

void     playerPause(uint8_t pause);
uint8_t  playerPaused(void);
void     playerSeek(int32_t deltaMs);
uint32_t playerPositionMs(void);
uint32_t playerDurationMs(void);
uint8_t  playerBufferedPercent(void);
uint8_t  playerBuffering(void);         // waiting for data
uint8_t  playerAudioFormat(void);       // MED_AFMT_*
uint32_t playerClockRate(void);         // sample rate (200 without sound)

void     playerStop(void);              // give back screen, sound, memory; CLOSE on the host

#endif
