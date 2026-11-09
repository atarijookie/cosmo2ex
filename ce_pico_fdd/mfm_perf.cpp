#include <cstring>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "defs.h"
#include "utils.h"
#include "mfm_stream.h"
#include "mfm_perf.h"

#if MFM_PERF_TEST

extern volatile uint32_t mfmHalfCount, mfmUnderruns, mfmLateMax, mfmGapMaxUs;

#define PERF_REFILL_SIZE    1024        // same as MFM_BUFFER_HALF_SIZE
#define PERF_REPORT_MS      5000

typedef struct {
    MfmStream   stream;
    uint32_t    count;
    uint64_t    totalUs;
    uint32_t    maxUs;                  // since boot
    uint32_t    maxUsNow;               // since the last report
    uint32_t    minPlayUs;              // shortest time such refill takes to play out - the time the next refill has
} PerfStats;

static uint8_t perfTracks[MFM_PERF_KINDS][READTRACKDATA_SIZE_BYTES];
static uint16_t perfOut[PERF_REFILL_SIZE];
static PerfStats perf[MFM_PERF_KINDS];
static volatile bool perfReady = false;
static uint32_t perfClockMhz;
static int perfNextKind = 0;

static uint32_t refillCount, refillMaxUs;
static uint64_t refillTotalUs;

static MfmStream perfSeek;
static uint32_t perfSeekMinUs = 0xffffffff, perfSeekMaxUs = 0, perfLastSeekMs = 0;

// track building - what the host encoder will do
static uint8_t* pOut;
static uint32_t builderRandom = 0x1234567;

static uint8_t randomByte(void)
{
    builderRandom ^= builderRandom << 13;
    builderRandom ^= builderRandom >> 17;
    builderRandom ^= builderRandom << 5;
    return (uint8_t) builderRandom;
}

static void putBytes(uint8_t val, int count)
{
    for(int i=0; i<count; i++) {
        *pOut++ = val;
    }
}

static void putSync(void)
{
    for(int i=0; i<3; i++) {
        *pOut++ = MFM_ESC;
        *pOut++ = MFM_OP_SYNC_A1;
    }
}

static void putCellTime(uint16_t cellTime)
{
    *pOut++ = MFM_ESC;
    *pOut++ = MFM_OP_CELL_TIME;
    *pOut++ = cellTime >> 8;
    *pOut++ = cellTime & 0xff;
}

static void putSectorStart(int sector)
{
    *pOut++ = MFM_ESC;                  // sector mark
    *pOut++ = MFM_OP_SECTOR;
    *pOut++ = 0;
    *pOut++ = 0;
    *pOut++ = sector;

    putBytes(0x00, 12);                 // ID field; CRC value doesn't matter for the timing
    putSync();
    *pOut++ = 0xFE;
    *pOut++ = 0;
    *pOut++ = 0;
    *pOut++ = sector;
    *pOut++ = 2;
    *pOut++ = 0x12;
    *pOut++ = 0x34;
    putBytes(0x4e, 22);

    putBytes(0x00, 12);                 // data field start
    putSync();
}

static void finishTrack(uint8_t* track)
{
    *pOut++ = MFM_ESC;
    *pOut++ = MFM_OP_END;

    uint32_t size = pOut - track;
    track[0] = size >> 8;
    track[1] = size & 0xff;
    track[2] = MFM_STREAM_VERSION;
    track[3] = 0;
}

// 9 sectors, data in a raw run. zeroData: flux in every 2nd cell of the data, the most intervals per data byte.
static void buildStandardTrack(uint8_t* track, bool zeroData)
{
    pOut = track + MFM_STREAM_HEADER_SIZE;
    putBytes(0x4e, 60);

    for(int sector=1; sector<=9; sector++) {
        putSectorStart(sector);

        *pOut++ = MFM_ESC;
        *pOut++ = MFM_OP_RAW;
        *pOut++ = 515 >> 8;
        *pOut++ = 515 & 0xff;
        *pOut++ = 0xFB;

        for(int i=0; i<512 + 2; i++) {
            *pOut++ = zeroData ? 0 : randomByte();
        }

        putBytes(0x4e, 40);
    }

    finishTrack(track);
}

// The worst an STX track can ask for: all 9 sectors with a cell time change every 16 bytes (+4.7 %, -4.8 % as in the
// Macrodos table) and all the bytes fuzzy, so every byte goes through the slowest path.
static void buildWorstStxTrack(uint8_t* track)
{
    pOut = track + MFM_STREAM_HEADER_SIZE;
    putBytes(0x4e, 60);

    for(int sector=1; sector<=9; sector++) {
        putCellTime(MFM_CELL_TIME_NOMINAL);
        putSectorStart(sector);
        *pOut++ = 0xFB;

        for(int block=0; block<32; block++) {
            putCellTime((block & 1) ? 33507 : 30469);

            *pOut++ = MFM_ESC;
            *pOut++ = MFM_OP_FUZZY;
            *pOut++ = 16;

            for(int i=0; i<16; i++) {
                *pOut++ = randomByte();
                *pOut++ = randomByte();
            }
        }

        putCellTime(MFM_CELL_TIME_NOMINAL);
        *pOut++ = randomByte();         // CRC
        *pOut++ = randomByte();
        putBytes(0x4e, 40);
    }

    finishTrack(track);
}

const uint8_t* mfmPerfTrack(int kind)
{
    return perfTracks[kind];
}

void mfmPerfInit(void)
{
    perfClockMhz = clock_get_hz(clk_sys) / 1000000;

    buildStandardTrack(perfTracks[0], false);
    buildWorstStxTrack(perfTracks[1]);
    buildStandardTrack(perfTracks[2], true);

    for(int i=0; i<MFM_PERF_KINDS; i++) {
        memset(&perf[i], 0, sizeof(perf[i]));
        perf[i].minPlayUs = 0xffffffff;
        perf[i].stream.random = 0x89abcdef;
        mfmStreamStart(&perf[i].stream, perfTracks[i], READTRACKDATA_SIZE_BYTES);
    }

    perfReady = true;
}

void __not_in_flash_func(mfmPerfOnRefill)(void)
{
    if(!perfReady) {
        return;
    }

    PerfStats* p = &perf[perfNextKind];
    perfNextKind = (perfNextKind + 1) % MFM_PERF_KINDS;

    if(p->stream.ended) {               // the track restart: no time left in the track, start it again
        mfmStreamStart(&p->stream, p->stream.track, READTRACKDATA_SIZE_BYTES);
    }

    uint32_t start = time_us_32();
    mfmStreamFill(&p->stream, perfOut, PERF_REFILL_SIZE);
    uint32_t us = time_us_32() - start;

    uint32_t ticks = 0;                 // how long this refill takes to play out
    for(int i=0; i<PERF_REFILL_SIZE; i++) {
        ticks += perfOut[i] + 1;
    }
    uint32_t playUs = ticks / perfClockMhz;

    p->count++;
    p->totalUs += us;
    p->maxUs = MAX(p->maxUs, us);
    p->maxUsNow = MAX(p->maxUsNow, us);
    p->minPlayUs = MIN(p->minPlayUs, playUs);
}

void __not_in_flash_func(mfmPerfRecordRefill)(uint32_t us)
{
    refillCount++;
    refillTotalUs += us;
    refillMaxUs = MAX(refillMaxUs, us);
}

void mfmPerfLoop(void)
{
    uint32_t now = millis();

    if(!perfReady || (now - perfLastSeekMs) < 100) {
        return;
    }
    perfLastSeekMs = now;

    perfSeek.random = 0x89abcdef;
    mfmStreamStart(&perfSeek, perfTracks[1], READTRACKDATA_SIZE_BYTES);

    uint32_t start = time_us_32();
    mfmStreamSeek(&perfSeek, 199000);   // the longest seek: to the end of the longest stream (IRQs stay on - the max includes them)
    uint32_t us = time_us_32() - start;

    perfSeekMinUs = MIN(perfSeekMinUs, us);
    perfSeekMaxUs = MAX(perfSeekMaxUs, us);
}

// load % = the longest refill / the shortest time the refill has before the next one is needed
void mfmPerfReport(void)
{
    static const char* names[MFM_PERF_KINDS] = {"std", "stx", "flux"};

    debug("mfm perf @ %u MHz: halves %u underruns %u | IRQ late max %u intervals, IRQ gap max %u us | refill avg %u max %u us | seek min %u max %u us\n",
          (unsigned) perfClockMhz, (unsigned) mfmHalfCount, (unsigned) mfmUnderruns,
          (unsigned) mfmLateMax, (unsigned) mfmGapMaxUs,
          (unsigned) (refillCount ? (refillTotalUs / refillCount) : 0), (unsigned) refillMaxUs,
          (unsigned) perfSeekMinUs, (unsigned) perfSeekMaxUs);

    for(int i=0; i<MFM_PERF_KINDS; i++) {
        PerfStats* p = &perf[i];
        uint32_t maxNow = p->maxUsNow;
        p->maxUsNow = 0;

        debug("  %-4s: avg %u us, max %u us (last period %u us), play min %u us, load %u %%, stream %u B\n",
              names[i], (unsigned) (p->count ? (p->totalUs / p->count) : 0), (unsigned) p->maxUs, (unsigned) maxNow,
              (unsigned) p->minPlayUs, (unsigned) (p->minPlayUs ? (p->maxUs * 100 / p->minPlayUs) : 0),
              (unsigned) ((perfTracks[i][0] << 8) | perfTracks[i][1]));
    }
}

#else

void mfmPerfInit(void) {}
void mfmPerfOnRefill(void) {}
void mfmPerfRecordRefill(uint32_t us) {}
void mfmPerfLoop(void) {}
void mfmPerfReport(void) {}
const uint8_t* mfmPerfTrack(int kind) { return nullptr; }

#endif
