#include "pico/stdlib.h"

#include "mfm_stream.h"

#define MFM_FILLER_CELLS    0x5555          // flux in every 2nd cell, 4 us. Starts with no flux, so never 1 cell after the previous flux

static uint16_t mfmTable[256];              // data byte -> 16 cells, with first clock bit as if the previous data bit was 0
static uint32_t cellTimeToTicksQ32;         // 1/16 ns -> PWM ticks, 32 fractional bits
static uint32_t ticksPerUsQ16;
static uint16_t fillerTop;

static inline uint32_t cellTimeToTicksQ16(uint32_t cellTime)
{
    return (uint32_t) (((uint64_t) cellTime * cellTimeToTicksQ32 + 0x8000) >> 16);     // rounded, so 2 us is exact, not a fraction of a tick less
}

void mfmStreamGlobalInit(uint32_t pwmClockHz)
{
    cellTimeToTicksQ32 = (uint32_t) ((((uint64_t) pwmClockHz) << 32) / 16000000000ULL);
    ticksPerUsQ16 = (uint32_t) ((((uint64_t) pwmClockHz) << 16) / 1000000);
    fillerTop = (uint16_t) (pwmClockHz / 250000 - 1);

    for(int val=0; val<256; val++) {
        uint16_t cells = 0;
        int prev = 0;

        for(int bit=7; bit>=0; bit--) {
            int data = (val >> bit) & 1;
            int clock = (!prev && !data) ? 1 : 0;

            cells = (cells << 2) | (clock << 1) | data;
            prev = data;
        }

        mfmTable[val] = cells;
    }
}

void __not_in_flash_func(mfmStreamStart)(MfmStream* s, const uint8_t* track, uint32_t trackBufferSize)
{
    s->track            = track;
    s->pos              = MFM_STREAM_HEADER_SIZE;
    s->word             = 0;
    s->cells            = 0;
    s->cellsSinceFlux   = 1;                // as if the last flux was just before: the first interval can't be just 1 cell, 2 us
    s->prevBit          = 0;
    s->rawLeft          = 0;
    s->fuzzyLeft        = 0;
    s->cellTicksQ16     = cellTimeToTicksQ16(MFM_CELL_TIME_NOMINAL);
    s->tickRemainder    = 0;
    s->random          |= 1;                // keep it random between revolutions, but xorshift state must not be 0

    uint32_t end = (((uint32_t) track[0]) << 8) | track[1];
    s->end = (end < trackBufferSize) ? end : trackBufferSize;
    s->ended = (track[2] != MFM_STREAM_VERSION);
}

static inline uint32_t encodeByte(MfmStream* s, uint8_t val)
{
    uint32_t cells = mfmTable[val];

    if(s->prevBit) {
        cells &= 0x7fff;                    // previous data bit 1 - no clock bit before this byte
    }

    s->prevBit = val & 1;
    return cells;
}

static uint32_t __not_in_flash_func(endOfStream)(MfmStream* s)
{
    s->ended = true;
    s->cellTicksQ16 = cellTimeToTicksQ16(MFM_CELL_TIME_NOMINAL);
    return MFM_FILLER_CELLS;
}

// next 16 cells of the stream
static uint32_t __not_in_flash_func(nextCells)(MfmStream* s)
{
    if(s->ended) {
        return MFM_FILLER_CELLS;
    }

    const uint8_t* track = s->track;

    while(true) {
        if(s->rawLeft > 0) {
            if(s->pos >= s->end) {
                return endOfStream(s);
            }

            s->rawLeft--;
            return encodeByte(s, track[s->pos++]);
        }

        if(s->fuzzyLeft > 0) {
            if(s->pos + 2 > s->end) {
                return endOfStream(s);
            }

            uint32_t rnd = s->random;       // xorshift32
            rnd ^= rnd << 13;
            rnd ^= rnd >> 17;
            rnd ^= rnd << 5;
            s->random = rnd;

            uint8_t data = track[s->pos++];
            uint8_t mask = track[s->pos++];

            s->fuzzyLeft--;
            return encodeByte(s, (data & mask) | (rnd & ~mask));
        }

        if(s->pos >= s->end) {
            return endOfStream(s);
        }

        uint8_t val = track[s->pos++];

        if(val != MFM_ESC) {                // plain data byte
            return encodeByte(s, val);
        }

        if(s->pos >= s->end) {
            return endOfStream(s);
        }

        uint8_t op = track[s->pos++];

        switch(op) {
            case MFM_OP_LITERAL:
                return encodeByte(s, MFM_ESC);

            case MFM_OP_SYNC_A1:
                s->prevBit = 1;
                return 0x4489;

            case MFM_OP_SYNC_C2:
                s->prevBit = 0;
                return 0x5224;

            case MFM_OP_CELL_TIME:
                if(s->pos + 2 > s->end) {
                    return endOfStream(s);
                }

                s->cellTicksQ16 = cellTimeToTicksQ16((((uint32_t) track[s->pos]) << 8) | track[s->pos + 1]);
                s->pos += 2;
                break;

            case MFM_OP_FUZZY:
                if(s->pos + 1 > s->end) {
                    return endOfStream(s);
                }

                s->fuzzyLeft = track[s->pos++];
                if(s->fuzzyLeft == 0) {
                    s->fuzzyLeft = 256;
                }
                break;

            case MFM_OP_SECTOR:
                if(s->pos + 3 > s->end) {
                    return endOfStream(s);
                }

                s->side     = track[s->pos++];
                s->trackNo  = track[s->pos++];
                s->sector   = track[s->pos++];
                break;

            case MFM_OP_RAW:
                if(s->pos + 2 > s->end) {
                    return endOfStream(s);
                }

                s->rawLeft = (((uint32_t) track[s->pos]) << 8) | track[s->pos + 1];
                s->pos += 2;
                break;

            default:                        // MFM_OP_END, or something we don't know - nothing more can be trusted
                return endOfStream(s);
        }
    }
}

void __not_in_flash_func(mfmStreamFill)(MfmStream* s, uint16_t* out, int count)
{
    uint32_t word = s->word;
    uint32_t cells = s->cells;
    uint32_t since = s->cellsSinceFlux;
    uint32_t remainder = s->tickRemainder;

    for(int i=0; i<count; ) {
        if(word == 0) {                     // no flux in the rest of the cells? count them, get next cells
            since += cells;
            word = nextCells(s) << 16;
            cells = 16;
            continue;
        }

        uint32_t advance = __builtin_clz(word) + 1;     // cells up to and including the next flux

        uint32_t ticks = (since + advance) * s->cellTicksQ16 + remainder;
        out[i++] = (uint16_t) ((ticks >> 16) - 1);      // PWM period is TOP + 1
        remainder = ticks & 0xffff;

        since = 0;
        cells -= advance;
        word <<= advance;
    }

    s->word = word;
    s->cells = cells;
    s->cellsSinceFlux = since;
    s->tickRemainder = remainder;
}

// Goes through the stream the same way as streaming it, just without making the intervals. Position is found by time,
// so it works for any track layout and any cell times. Continues at a byte boundary, up to 32 us of the stream after timeUs.
void __not_in_flash_func(mfmStreamSeek)(MfmStream* s, uint32_t timeUs)
{
    uint64_t target = ((uint64_t) timeUs) * ticksPerUsQ16;
    uint64_t now = 0;

    while(now < target && !s->ended) {
        uint32_t cellTicks = s->cellTicksQ16;
        nextCells(s);
        now += 16 * (uint64_t) cellTicks;
    }
}

void __not_in_flash_func(mfmStreamFiller)(uint16_t* out, int count)
{
    for(int i=0; i<count; i++) {
        out[i] = fillerTop;
    }
}

int mfmStreamNextRawRun(const uint8_t* track, uint32_t trackBufferSize, uint32_t* pos, uint32_t* runSize, int* side, int* trackNo, int* sector)
{
    uint32_t end = (((uint32_t) track[0]) << 8) | track[1];
    end = (end < trackBufferSize) ? end : trackBufferSize;

    if(track[2] != MFM_STREAM_VERSION) {
        return -1;
    }

    uint32_t p = (*pos < MFM_STREAM_HEADER_SIZE) ? MFM_STREAM_HEADER_SIZE : *pos;

    while(p < end) {
        if(track[p++] != MFM_ESC) {         // data byte
            continue;
        }

        if(p >= end) {
            return -1;
        }

        uint8_t op = track[p++];
        uint32_t size;

        switch(op) {
            case MFM_OP_LITERAL:
            case MFM_OP_SYNC_A1:
            case MFM_OP_SYNC_C2:
                break;

            case MFM_OP_CELL_TIME:
                p += 2;
                break;

            case MFM_OP_FUZZY:
                if(p >= end) {
                    return -1;
                }

                size = track[p];
                p += 1 + 2 * ((size == 0) ? 256 : size);
                break;

            case MFM_OP_SECTOR:
                if(p + 3 > end) {
                    return -1;
                }

                *side       = track[p];
                *trackNo    = track[p + 1];
                *sector     = track[p + 2];
                p += 3;
                break;

            case MFM_OP_RAW:
                if(p + 2 > end) {
                    return -1;
                }

                size = (((uint32_t) track[p]) << 8) | track[p + 1];
                p += 2;

                if(p + size > end) {                // cut off by the stream end? can't be written into
                    return -1;
                }

                *runSize = size;
                *pos = p + size;
                return (int) p;

            default:                                // MFM_OP_END, or something we don't know
                return -1;
        }
    }

    return -1;
}
