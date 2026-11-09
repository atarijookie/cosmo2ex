// Helpers for the tests of the track stream: tracks as ce_cores/fdd makes them (see ce_pico_fdd/encoded_track_format.txt)
// and written sectors as the device captures them.

#ifndef MFM_TEST_TRACK_H
#define MFM_TEST_TRACK_H

#include <vector>
#include <cstdint>
#include <cstring>

#include "defs.h"
#include "mfm_stream.h"

// the CRC of ID and data fields, from the start as the FDC does it - with the 3x A1 in it
inline uint16_t testCrc(const std::vector<uint8_t>& bytes)
{
    uint16_t crc = 0xffff;
    std::vector<uint8_t> all = {0xa1, 0xa1, 0xa1};
    all.insert(all.end(), bytes.begin(), bytes.end());

    for(uint8_t val : all) {
        for(int i=0; i<8; i++) {
            crc = ((crc << 1) ^ ((((crc >> 8) ^ (val << i)) & 0x0080) ? 0x1021 : 0));
        }
    }

    return crc;
}

// the 515 B raw run of a sector with this data
inline std::vector<uint8_t> testSectorRun(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> run = {0xfb};
    run.insert(run.end(), data.begin(), data.end());

    uint16_t crc = testCrc(run);
    run.push_back(crc >> 8);
    run.push_back(crc & 0xff);
    return run;
}

// sector data different per sector, with all kinds of bytes in it - also F5
inline std::vector<uint8_t> testSectorData(uint8_t seed)
{
    std::vector<uint8_t> data(512);
    uint32_t rnd = seed * 2654435761u + 1;

    for(int i=0; i<512; i++) {
        rnd = rnd * 1103515245 + 12345;
        data[i] = rnd >> 16;
    }

    return data;
}

// noDataSector: this sector has its header, but no data part. Returned in a whole track buffer, zeros after the stream.
inline std::vector<uint8_t> buildTestTrack(int track, int side, int spt, int noDataSector = -1)
{
    std::vector<uint8_t> t = {0, 0, MFM_STREAM_VERSION, 0};

    auto put = [&](uint8_t val, int count) { t.insert(t.end(), count, val); };
    auto sync = [&](void) { for(int i=0; i<3; i++) { t.push_back(MFM_ESC); t.push_back(MFM_OP_SYNC_A1); } };

    put(0x4e, 60);

    for(int s=1; s<=spt; s++) {
        t.insert(t.end(), {MFM_ESC, MFM_OP_SECTOR, (uint8_t) side, (uint8_t) track, (uint8_t) s});
        put(0x00, 12);
        sync();

        uint16_t idCrc = testCrc({0xfe, (uint8_t) track, (uint8_t) side, (uint8_t) s, 2});
        for(uint8_t val : {0xfe, track, side, s, 2, idCrc >> 8, idCrc & 0xff}) {
            if(val == MFM_ESC) {
                t.push_back(MFM_ESC);
                t.push_back(MFM_OP_LITERAL);
            } else {
                t.push_back(val);
            }
        }

        put(0x4e, 22);

        if(s != noDataSector) {
            put(0x00, 12);
            sync();

            std::vector<uint8_t> run = testSectorRun(testSectorData(s));
            t.insert(t.end(), {MFM_ESC, MFM_OP_RAW, MFM_SECTOR_RUN_SIZE >> 8, MFM_SECTOR_RUN_SIZE & 0xff});
            t.insert(t.end(), run.begin(), run.end());
        }

        put(0x4e, 40);
    }

    t.push_back(MFM_ESC);
    t.push_back(MFM_OP_END);

    t[0] = t.size() >> 8;
    t[1] = t.size() & 0xff;

    t.resize(READTRACKDATA_SIZE_BYTES, 0);
    return t;
}

// offset of the raw run of this sector in the track, -1 if there's none
inline int testFindRun(const std::vector<uint8_t>& t, int sector)
{
    uint32_t pos = 0, size = 0;
    int markSide = -1, markTrack = -1, markSector = -1;

    while(true) {
        int offset = mfmStreamNextRawRun(t.data(), t.size(), &pos, &size, &markSide, &markTrack, &markSector);

        if(offset < 0 || markSector == sector) {
            return offset;
        }
    }
}

// What mfm_write captures from the ST: 4 interval symbols in a byte. Made as the v1 encoder of the host did it - on the
// hardware that encoder's stream was read and written back by the WD1772 without errors.
struct SymbolEncoder {
    uint8_t threeBits = 1, times = 0, timesCnt = 0;
    std::vector<uint8_t> out;
    std::vector<int> intervals;

    void symbol(uint8_t time) {
        intervals.push_back(2 + 2 * time);
        times = (times << 2) | time;
        if(++timesCnt == 4) {
            timesCnt = 0;
            out.push_back(times);
        }
    }

    void byte(uint8_t val) {
        for(int i=0; i<8; i++) {
            threeBits = ((threeBits << 1) | ((val & 0x80) >> 7)) & 7;
            val <<= 1;

            switch(threeBits) {
                case 0: case 3: case 7: symbol(MFM_4US); break;
                case 1: case 4:         symbol(MFM_6US); break;
                case 5:                 symbol(MFM_8US); break;
            }
        }
    }

    void bytes(const std::vector<uint8_t>& vals) {
        for(uint8_t val : vals) {
            byte(val);
        }
    }

    void syncA1(void) {
        symbol((threeBits == 0 || threeBits == 4) ? MFM_6US : ((threeBits == 2 || threeBits == 6) ? MFM_8US : MFM_4US));
        symbol(MFM_8US);
        symbol(MFM_6US);
        symbol(MFM_8US);
        symbol(MFM_6US);
        threeBits = 1;
    }
};

// the symbols of a write: 12x 00, 3x A1, then these bytes, then some of the gap (the ST turns the write off a bit after the CRC)
inline std::vector<uint8_t> testWriteSymbols(const std::vector<uint8_t>& bytes, int gapBytes = 1)
{
    SymbolEncoder enc;
    enc.bytes(std::vector<uint8_t>(12, 0x00));
    for(int i=0; i<3; i++) {
        enc.syncA1();
    }
    enc.bytes(bytes);
    enc.bytes(std::vector<uint8_t>(gapBytes, 0x4e));
    return enc.out;
}

#endif
