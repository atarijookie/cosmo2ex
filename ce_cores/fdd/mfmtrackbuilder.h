// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef MFMTRACKBUILDER_H
#define MFMTRACKBUILDER_H

#include <stdint.h>

// The v2 track stream as sent to the device: header + data bytes + F5 escapes. The device does the MFM encoding,
// see ce_pico_fdd/encoded_track_format.txt. These values must match ce_pico_fdd/mfm_stream.h.
#define MFM_STREAM_VERSION      2
#define MFM_STREAM_HEADER_SIZE  4       // 0..1: offset of stream end (from track start), 2: version, 3: reserved

#define MFM_ESC                 0xF5
#define MFM_OP_LITERAL          0x00    // data byte 0xF5
#define MFM_OP_SYNC_A1          0xA1    // A1 with missing clock
#define MFM_OP_SYNC_C2          0xC2    // C2 with missing clock
#define MFM_OP_CELL_TIME        0x10    // hi lo: bit cell time in 1/16 ns
#define MFM_OP_FUZZY            0x20    // n (0 = 256), n * (data, mask): mask bit 1 keeps the data bit, 0 makes it random
#define MFM_OP_SECTOR           0x40    // side track sector: the sector following in the stream
#define MFM_OP_RAW              0x50    // hi lo, n data bytes without escapes
#define MFM_OP_END              0xFF

#define MFM_SECTOR_RUN_SIZE     515     // raw run of a 512 B sector: DAM, data, CRC - the device overwrites it on sector write

class MfmTrackBuilder
{
public:
    MfmTrackBuilder();

    void begin(uint8_t *buffer, int bufferSize);    // start a new track in this buffer

    void data(uint8_t val);                         // data byte, escaped when needed
    void data(uint8_t val, int count);
    void syncA1(void);
    void syncC2(void);
    void cellTime(uint16_t time);
    void sectorMark(int side, int track, int sector);
    void rawRun(const uint8_t *bytes, int count);
    void fuzzyRun(const uint8_t *dataAndMaskPairs, int count);  // count: pairs, 1..256

    int  finish(void);                              // end mark + header, returns the stream size in bytes
    bool overflowed(void);                          // something didn't fit - all items from that one on were left out

private:
    bool room(int count);
    void put(uint8_t val);

    uint8_t *start;
    int  size;
    int  used;
    bool overflow;
};

#endif
