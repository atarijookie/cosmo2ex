#ifndef __MFM_STREAM_H__
#define __MFM_STREAM_H__

#include <cstdint>

// v2 track stream decoder: turns a track stream (data bytes + F5 escapes, see encoded_track_format.txt) into PWM TOP
// values for the read stream DMA ring. The MFM encoding happens here, a few bytes at a time as the ring needs them -
// nothing is encoded in advance, so there's nothing to redo on track start or after a seek.

#define MFM_STREAM_VERSION      2
#define MFM_STREAM_HEADER_SIZE  4       // 0..1: offset of stream end (from track start), 2: version, 3: reserved

#define MFM_ESC                 0xF5
#define MFM_OP_LITERAL          0x00    // data byte 0xF5
#define MFM_OP_SYNC_A1          0xA1    // A1 with missing clock (0x4489)
#define MFM_OP_SYNC_C2          0xC2    // C2 with missing clock (0x5224)
#define MFM_OP_CELL_TIME        0x10    // hi lo: bit cell time in 1/16 ns
#define MFM_OP_FUZZY            0x20    // n (0 = 256), n * (data, mask): mask bit 1 keeps the data bit, 0 makes it random
#define MFM_OP_SECTOR           0x40    // side track sector: the sector following in the stream
#define MFM_OP_RAW              0x50    // hi lo, n data bytes without escapes
#define MFM_OP_END              0xFF

#define MFM_CELL_TIME_NOMINAL   32000   // 2 us
#define MFM_SECTOR_RUN_SIZE     515     // raw run of a 512 B sector: DAM, data, CRC - the sector write overwrites it in place

typedef struct {
    const uint8_t*  track;
    uint32_t        pos;            // offset of the next stream byte
    uint32_t        end;            // stream end, never beyond the track buffer

    uint32_t        word;           // cells not turned into intervals yet, next one in the highest bit
    uint32_t        cells;          // how many of them
    uint32_t        cellsSinceFlux;
    uint8_t         prevBit;        // last data bit, the next clock bit depends on it

    uint32_t        rawLeft;
    uint32_t        fuzzyLeft;

    uint32_t        cellTicksQ16;   // bit cell time in PWM ticks, 16 fractional bits
    uint32_t        tickRemainder;  // fraction of a tick left from the previous interval, so timing doesn't drift

    uint32_t        random;
    bool            ended;          // end of stream or bad stream - only 4 us filler now

    uint8_t         side, trackNo, sector;      // from the last sector mark
} MfmStream;

void mfmStreamGlobalInit(uint32_t pwmClockHz);                                      // PWM runs at this clock with divider 1
void mfmStreamStart(MfmStream* s, const uint8_t* track, uint32_t trackBufferSize);  // from the track start
void mfmStreamFill(MfmStream* s, uint16_t* out, int count);                         // next 'count' PWM TOP values
void mfmStreamSeek(MfmStream* s, uint32_t timeUs);                                  // after mfmStreamStart: skip to this time after track start
void mfmStreamFiller(uint16_t* out, int count);                                     // PWM TOP values of 4 us intervals

// Find the next raw run of a track, from offset *pos (0 = from the start), for writing into it. Returns its offset, *pos
// is set beyond it for the next call; -1 when there is no more. side, trackNo, sector: from the last sector mark before it, -1 if none.
int mfmStreamNextRawRun(const uint8_t* track, uint32_t trackBufferSize, uint32_t* pos, uint32_t* runSize, int* side, int* trackNo, int* sector);

#endif
