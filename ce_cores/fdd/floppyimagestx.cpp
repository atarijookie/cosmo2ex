// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <string.h>
#include <algorithm>
#include <map>

#include "../misc/debug.h"
#include "floppyimagestx.h"
#include "mfmtrackbuilder.h"

#define STX_MAX_FILE_SIZE       (16 * 1024 * 1024)
#define STX_MAX_TRACKS          85
#define STX_HEADER_SIZE         16
#define STX_BLOCK_SIZE          16

#define STX_TRACK_HAS_SECTORS   0x01        // otherwise the track is just its sectors, 512 B each
#define STX_TRACK_HAS_IMAGE     0x40
#define STX_TRACK_IMAGE_SYNC    0x80        // the track image has a sync position before its size

#define STX_SECTOR_VARIABLE     0x01        // variable bit rate: has timing data
#define STX_SECTOR_CRC_ERROR    0x08
#define STX_SECTOR_NOT_FOUND    0x10        // has no data. With CRC error: the CRC error is in the ID field
#define STX_SECTOR_DELETED      0x20
#define STX_SECTOR_FUZZY        0x80

#define ID_MARK                 0xfe
#define DAM_MARK                0xfb
#define DELETED_DAM_MARK        0xf8

#define STANDARD_TRACK_BYTES    6250        // 200 ms at 2 us bit cell
#define MIN_TRACK_BYTES         5800        // the bit rate is changed to fit the track into a rotation, but just this far.
#define MAX_TRACK_BYTES         6800        // Outside of it the size is rather wrong than a floppy with so different bit rate.
#define MAX_TIMELINE_BYTES      8000

#define GAP_BEFORE_SYNC         12
#define GAP_AFTER_ID            22
#define GAP_BEFORE_DAM_SYNC     12

#define ID_FIELD_BYTES          7           // ID mark, track, side, sector, size, 2 x CRC
#define ID_MARK_SEARCH_RANGE    48          // a damaged ID field in the track image: looking just for its mark, this close
#define DAM_SEARCH_RANGE        80

// Macrodos / Speedlock images of old Pasti versions (revision 0) have no timing data, they use this (from Pasti itself, via Hatari)
static const uint8_t defaultTimingData[64] = {
    0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,
    0x00,0x85,0x00,0x85,0x00,0x85,0x00,0x85,0x00,0x85,0x00,0x85,0x00,0x85,0x00,0x85,
    0x00,0x79,0x00,0x79,0x00,0x79,0x00,0x79,0x00,0x79,0x00,0x79,0x00,0x79,0x00,0x79,
    0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f,0x00,0x7f
};

static inline uint16_t readU16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static inline uint32_t readU32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (((uint32_t) p[3]) << 24);
}

// CRC on the floppy, with the 3x A1 sync before the bytes in it
static uint16_t crcAfterSync(const uint8_t *bytes, int count, uint16_t crc = 0xcdb4)
{
    for(int i=0; i<count; i++) {
        crc ^= ((uint16_t) bytes[i]) << 8;

        for(int bit=0; bit<8; bit++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }

    return crc;
}

FloppyImageStx::~FloppyImageStx()
{
    clear();
}

bool FloppyImageStx::open(const char *fileName)
{
    loadedFlag = false;

    if(!FloppyImage::open(fileName)) {
        return false;
    }

    bool res = loadImageIntoMemory();
    close();

    if(!res || !parse()) {
        logFdd(LOG_ERROR, "STX Image %s is not valid, not loading", fileName);
        tracks.clear();
        clear();
        return false;
    }

    loadedFlag = true;

    logFdd(LOG_DEBUG, "STX Image opened: %s - %d tracks, %d sides, %d sectors per track", fileName, params.tracksNo, params.sidesNo, params.sectorsPerTrack);
    return true;
}

bool FloppyImageStx::save(void)
{
    sectorsWritten = 0;
    return true;
}

bool FloppyImageStx::readNotWriteSector(bool readNotWrite, int track, int side, int sectorNo, uint8_t *buffer)
{
    return false;       // sector numbers mean nothing here, a track can have the same one many times; the device gets whole tracks
}

bool FloppyImageStx::isReadOnly(void)
{
    return true;
}

bool FloppyImageStx::parse(void)
{
    if(image.size < STX_HEADER_SIZE || image.size > STX_MAX_FILE_SIZE) {
        return false;
    }

    const uint8_t *header = image.data;

    if(memcmp(header, "RSY\0", 4) != 0 || readU16(header + 4) != 3) {   // not an STX image, e.g. a STinG module has the same extension
        logFdd(LOG_ERROR, "STX Image - bad file ID or version, this is not a floppy image");
        return false;
    }

    int trackBlocks = header[10];
    int revision = header[11];

    tracks.clear();
    tracks.resize(STX_MAX_TRACKS * 2);

    uint32_t pos = STX_HEADER_SIZE;

    for(int i=0; i<trackBlocks; i++) {
        if(pos + STX_BLOCK_SIZE > (uint32_t) image.size) {
            logFdd(LOG_ERROR, "STX Image - file ends before track block %d", i);
            return false;
        }

        uint32_t blockSize = readU32(image.data + pos);

        if(blockSize < STX_BLOCK_SIZE || blockSize > (uint32_t) image.size - pos) {
            logFdd(LOG_ERROR, "STX Image - track block %d has bad size %u", i, blockSize);
            return false;
        }

        if(!parseTrack(pos, blockSize, revision)) {
            logFdd(LOG_ERROR, "STX Image - track block %d is damaged", i);
            return false;
        }

        pos += blockSize;
    }

    // geometry: only for what's shown and how many tracks are sent
    int lastTrack = -1;
    int sides = 1;
    std::map<int, int> sectorCounts;

    for(int t=0; t<STX_MAX_TRACKS; t++) {
        for(int s=0; s<2; s++) {
            const Track &track = tracks[2 * t + s];

            if(track.present) {
                lastTrack = t;
                sides = (s == 1) ? 2 : sides;

                if(!track.sectors.empty()) {
                    sectorCounts[track.sectors.size()]++;
                }
            }
        }
    }

    if(lastTrack < 0) {
        logFdd(LOG_ERROR, "STX Image - no tracks in it");
        return false;
    }

    int spt = 0, sptCount = 0;
    for(auto &item : sectorCounts) {
        if(item.second > sptCount) {
            spt = item.first;
            sptCount = item.second;
        }
    }

    params.tracksNo         = lastTrack + 1;
    params.sidesNo          = sides;
    params.sectorsPerTrack  = spt;
    params.isInit           = true;

    return true;
}

bool FloppyImageStx::parseTrack(uint32_t blockStart, uint32_t blockSize, int revision)
{
    const uint8_t *block = image.data + blockStart;

    uint32_t fuzzySize  = readU32(block + 4);
    int sectorCount     = readU16(block + 8);
    int flags           = readU16(block + 10);
    int mfmSize         = readU16(block + 12);
    int trackNumber     = block[14];

    int t = trackNumber & 0x7f;
    int s = trackNumber >> 7;

    if(t >= STX_MAX_TRACKS) {           // can't be streamed
        logFdd(LOG_WARNING, "STX Image - skipping track %d", t);
        return true;
    }

    Track track;
    track.present = true;
    track.mfmSize = mfmSize;
    track.image = NULL;
    track.imageSize = 0;

    if((flags & STX_TRACK_HAS_SECTORS) == 0 && sectorCount > 0) {   // just sectors, no descriptors: size in bits, standard layout
        if(STX_BLOCK_SIZE + (uint64_t) sectorCount * 512 > blockSize) {
            return false;
        }

        track.mfmSize = mfmSize / 8;

        for(int i=0; i<sectorCount; i++) {
            Sector sector = Sector();

            uint8_t id[5] = {ID_MARK, (uint8_t) t, (uint8_t) s, (uint8_t) (i + 1), 2};

            sector.bitPosition  = (60 + GAP_BEFORE_SYNC + 4 + i * 614) * 8;     // as in the standard track, see MfmCachedImage
            sector.idTrack      = id[1];
            sector.idHead       = id[2];
            sector.idSector     = id[3];
            sector.idSize       = id[4];
            sector.idCrc        = crcAfterSync(id, 5);
            sector.size         = 512;
            sector.data         = block + STX_BLOCK_SIZE + i * 512;

            track.sectors.push_back(sector);
        }

        tracks[2 * t + s] = track;
        return true;
    }

    uint64_t fuzzyStart = STX_BLOCK_SIZE + (uint64_t) sectorCount * STX_BLOCK_SIZE;
    uint64_t trackData  = fuzzyStart + fuzzySize;

    if(trackData > blockSize) {
        return false;
    }

    uint64_t sectorsData = trackData;   // where the sector data would be if the offsets said so, for the timing data after them

    if(flags & STX_TRACK_HAS_IMAGE) {
        uint64_t pos = trackData + ((flags & STX_TRACK_IMAGE_SYNC) ? 2 : 0);    // the sync position isn't needed: the image starts at the index

        if(pos + 2 > blockSize) {
            return false;
        }

        track.imageSize = readU16(block + pos);
        pos += 2;

        if(pos + track.imageSize > blockSize) {
            return false;
        }

        track.image = block + pos;
        sectorsData = pos + track.imageSize;
    }

    uint64_t maxSectorEnd = sectorsData;
    uint64_t fuzzyPos = fuzzyStart;
    bool variable = false;

    for(int i=0; i<sectorCount; i++) {
        const uint8_t *d = block + STX_BLOCK_SIZE + i * STX_BLOCK_SIZE;

        Sector sector = Sector();
        sector.bitPosition  = readU16(d + 4);
        sector.readTime     = readU16(d + 6);
        sector.idTrack      = d[8];
        sector.idHead       = d[9];
        sector.idSector     = d[10];
        sector.idSize       = d[11];
        sector.idCrc        = (d[12] << 8) | d[13];     // this one is big endian
        sector.status       = d[14];

        if((sector.status & STX_SECTOR_NOT_FOUND) == 0) {
            sector.size = 128 << (sector.idSize & 3);

            uint64_t start = trackData + readU32(d);

            if(start + sector.size > blockSize) {
                return false;
            }

            sector.data = block + start;
            maxSectorEnd = std::max(maxSectorEnd, start + sector.size);

            if(sector.status & STX_SECTOR_FUZZY) {
                if(fuzzyPos + sector.size > trackData) {
                    return false;
                }

                sector.fuzzyMask = block + fuzzyPos;
                fuzzyPos += sector.size;
            }

            variable = variable || (sector.status & STX_SECTOR_VARIABLE);
        }

        track.sectors.push_back(sector);
    }

    if(variable) {
        const uint8_t *timing = defaultTimingData;
        uint64_t timingLeft = sizeof(defaultTimingData);

        if(revision == 2) {
            if(maxSectorEnd + 4 > blockSize) {      // 2 B flags, 2 B size, then the data
                return false;
            }

            timing = block + maxSectorEnd + 4;
            timingLeft = blockSize - maxSectorEnd - 4;
        }

        for(auto &sector : track.sectors) {
            if(sector.size == 0 || (sector.status & STX_SECTOR_VARIABLE) == 0) {
                continue;
            }

            sector.timing = timing;
            sector.timingBlocks = sector.size / 16;

            if((uint64_t) sector.timingBlocks * 2 > timingLeft) {
                if(revision == 2) {
                    return false;
                }
                sector.timingBlocks = timingLeft / 2;   // the table of old images is for 512 B
            }

            if(revision == 2) {
                timing += sector.timingBlocks * 2;
                timingLeft -= sector.timingBlocks * 2;
            }
        }
    }

    // the position on the track decides, not the order in the file
    std::stable_sort(track.sectors.begin(), track.sectors.end(), [](const Sector &a, const Sector &b) { return a.bitPosition < b.bitPosition; });

    tracks[2 * t + s] = track;
    return true;
}

// One byte position of the track being built.
struct TrackByte {
    uint8_t value;
    uint8_t mask;       // fuzzy bits: 0 in the mask is unstable
    uint8_t sync;       // 0: data byte, otherwise the byte with missing clock
    bool    fixed;      // not just a gap, a gap of other sector mustn't overwrite it (sectors can be in the data of other ones)
};

// 1 unit of timing data = 32 FDC cycles at 8 MHz, + 28 cycles for 16 bytes, which are 256 bit cells. Formula from Pasti, via Hatari.
static uint16_t timingToCellTime(uint32_t timing)
{
    uint32_t cycles = timing * 32 + 28;
    uint32_t cellTime = cycles * 1000 / 128;    // 125 ns per cycle, 16 units per ns, 256 cells

    return (uint16_t) std::min(cellTime, (uint32_t) 0xffff);
}

// Rebuilds the track: starts with the track image if it has one, otherwise with the gap, and puts the sectors to their
// positions. Each byte position is 32 us in a standard track; the stream gets the same bytes with the bit cell time changes.
bool FloppyImageStx::buildTrack(MfmTrackBuilder &builder, int track, int side)
{
    if(track < 0 || track >= STX_MAX_TRACKS || side < 0 || side > 1) {
        return true;
    }

    const Track &source = tracks[2 * track + side];

    if(!source.present || (source.sectors.empty() && source.image == NULL)) {   // not formatted: empty track, no ID field will be found
        return true;
    }

    int length = source.image ? source.imageSize : source.mfmSize;

    if(length <= 0) {
        length = STANDARD_TRACK_BYTES;
    }

    int rotationBytes = std::max(MIN_TRACK_BYTES, std::min(MAX_TRACK_BYTES, length));   // for the bit rate
    uint16_t baseCellTime = (uint16_t) (200000000 / rotationBytes);     // the track takes 1 rotation: 200 ms in 1/16 ns units, per byte = per 16 cells

    int timelineSize = MAX_TIMELINE_BYTES;

    TrackByte gapByte = {0x4e, 0xff, 0, false};
    std::vector<TrackByte> bytes(timelineSize, gapByte);
    std::vector<uint16_t> cellTimes(timelineSize, 0);   // 0: no change at this byte
    std::vector<int> sectorMarks(timelineSize, -1);

    if(source.image) {                  // the track as it was read, with the sync marks found: Read Track doesn't tell which A1 is one
        int count = std::min(source.imageSize, timelineSize);

        for(int i=0; i<count; i++) {
            bytes[i].value = source.image[i];
            bytes[i].fixed = true;
        }

        // Read Track gets its bytes aligned by the sync mark, so the first byte of 3 comes garbled: 14 A1 A1 FE
        for(int i=0; i + 3 < count; i++) {
            uint8_t val = source.image[i + 1], next = source.image[i + 3];

            if(val != source.image[i + 2]) {
                continue;
            }

            bool a1 = (val == 0xa1) && (next == ID_MARK || (next >= DELETED_DAM_MARK && next <= DAM_MARK));
            bool c2 = (val == 0xc2) && (next == 0xfc);

            if(a1 || c2) {
                bytes[i].value = val;
                bytes[i].sync = bytes[i + 1].sync = bytes[i + 2].sync = val;
            }
        }
    }

    auto put = [&](int pos, uint8_t val) {
        if(pos >= 0 && pos < timelineSize) {
            TrackByte item = {val, 0xff, 0, true};
            bytes[pos] = item;
        }
    };
    auto putGap = [&](int pos, uint8_t val) {
        if(pos >= 0 && pos < timelineSize && !bytes[pos].fixed) {
            bytes[pos].value = val;
        }
    };
    auto putSync = [&](int pos) {
        for(int i=0; i<3; i++) {
            put(pos + i, 0xa1);
            if(pos + i >= 0 && pos + i < timelineSize) {
                bytes[pos + i].sync = 0xa1;
            }
        }
    };

    int trackEnd = length;

    for(size_t index=0; index<source.sectors.size(); index++) {
        const Sector &sector = source.sectors[index];

        uint8_t idField[ID_FIELD_BYTES - 2] = {ID_MARK, sector.idTrack, sector.idHead, sector.idSector, sector.idSize};
        uint16_t idCrc = sector.idCrc;

        // CRC error in the ID field, but what's stored is a good one? Then there would be no error.
        if((sector.status & STX_SECTOR_NOT_FOUND) && (sector.status & STX_SECTOR_CRC_ERROR) && idCrc == crcAfterSync(idField, 5)) {
            idCrc ^= 0xffff;
        }

        int idPos = sector.bitPosition / 8 - 1;     // the bit position is just after the ID mark

        if(source.image) {
            // Where it really is in the track image: the bit position is time, the bytes of the image are off by tens of
            // bytes at the end of a track with higher bit rate. So the closest same ID field in the image. When Read Track
            // garbled it, then the closest ID mark - putting it elsewhere would make the track have this sector twice.
            int expected = (int) (((int64_t) idPos * source.imageSize) / STANDARD_TRACK_BYTES);
            int best = -1;

            for(int i=0; i + ID_FIELD_BYTES <= source.imageSize; i++) {
                if(memcmp(source.image + i, idField, 5) == 0 && (best < 0 || abs(i - expected) < abs(best - expected))) {
                    best = i;
                }
            }

            if(best < 0) {
                for(int i = std::max(expected - ID_MARK_SEARCH_RANGE, 2); i <= expected + ID_MARK_SEARCH_RANGE && i < source.imageSize; i++) {
                    bool idMark = source.image[i] == ID_MARK && source.image[i - 1] == 0xa1 && source.image[i - 2] == 0xa1;

                    if(idMark && (best < 0 || abs(i - expected) < abs(best - expected))) {
                        best = i;
                    }
                }
            }

            idPos = (best >= 0) ? best : expected;
        }

        for(int i=0; i<GAP_BEFORE_SYNC; i++) {
            putGap(idPos - 3 - GAP_BEFORE_SYNC + i, 0);
        }

        putSync(idPos - 3);
        for(int i=0; i<5; i++) {
            put(idPos + i, idField[i]);
        }
        put(idPos + 5, idCrc >> 8);
        put(idPos + 6, idCrc & 0xff);

        if(idPos >= 3 && idPos - 3 < timelineSize) {
            sectorMarks[idPos - 3] = index;
        }

        trackEnd = std::max(trackEnd, idPos + ID_FIELD_BYTES);

        if(sector.size == 0) {                      // header only - it will end with Record Not Found
            continue;
        }

        int syncPos = idPos + ID_FIELD_BYTES + GAP_AFTER_ID + GAP_BEFORE_DAM_SYNC;

        if(source.image) {                          // the data field in the track image, if it's there
            for(int i = idPos + ID_FIELD_BYTES; i < idPos + ID_FIELD_BYTES + DAM_SEARCH_RANGE && i + 4 <= source.imageSize; i++) {
                if(source.image[i + 1] == 0xa1 && source.image[i + 2] == 0xa1 &&     // the first byte of the mark is garbled
                   source.image[i + 3] >= DELETED_DAM_MARK && source.image[i + 3] <= DAM_MARK) {
                    syncPos = i;
                    break;
                }
            }
        }

        for(int i = idPos + ID_FIELD_BYTES + GAP_AFTER_ID; i < syncPos; i++) {
            putGap(i, 0);
        }

        putSync(syncPos);

        uint8_t dam = (sector.status & STX_SECTOR_DELETED) ? DELETED_DAM_MARK : DAM_MARK;
        int damPos = syncPos + 3;
        int dataPos = damPos + 1;

        put(damPos, dam);

        for(int i=0; i<sector.size; i++) {
            put(dataPos + i, sector.data[i]);

            if(sector.fuzzyMask && dataPos + i < timelineSize) {
                bytes[dataPos + i].mask = sector.fuzzyMask[i];
            }
        }

        uint16_t crc = crcAfterSync(&dam, 1);
        crc = crcAfterSync(sector.data, sector.size, crc);

        if(sector.status & STX_SECTOR_CRC_ERROR) {  // the file doesn't have the CRC of the data - it's just known that it was wrong
            crc ^= 0xffff;
        }

        int crcPos = dataPos + sector.size;
        put(crcPos, crc >> 8);
        put(crcPos + 1, crc & 0xff);

        trackEnd = std::max(trackEnd, crcPos + 2);

        // the bit rate of the data
        auto setCellTime = [&](int pos, uint16_t time) {
            if(pos >= 0 && pos < timelineSize) {
                cellTimes[pos] = time;
            }
        };

        if(sector.timing) {
            for(int block=0; block<sector.timingBlocks; block++) {
                setCellTime(dataPos + block * 16, timingToCellTime((sector.timing[block * 2] << 8) | sector.timing[block * 2 + 1]));
            }
            setCellTime(crcPos, baseCellTime);
        } else if(sector.readTime != 0) {
            setCellTime(dataPos, (uint16_t) std::min(sector.readTime * 1000u / sector.size, 0xffffu));   // us to 1/16 ns units, per byte = 16 cells
            setCellTime(crcPos, baseCellTime);
        }
    }

    trackEnd = std::min(trackEnd, timelineSize);

    if(trackEnd > rotationBytes + 100) {
        logFdd(LOG_WARNING, "STX Image track %d side %d: %d bytes don't fit into a rotation, the end of the track is lost", track, side, trackEnd);
    }

    // now it's just going through it. Fuzzy bytes go in runs together with their masks.
    if(baseCellTime != MFM_CELL_TIME_NOMINAL) {
        builder.cellTime(baseCellTime);
    }

    for(int i=0; i<trackEnd; ) {
        if(cellTimes[i] != 0) {
            builder.cellTime(cellTimes[i]);
        }

        if(sectorMarks[i] >= 0) {
            const Sector &sector = source.sectors[sectorMarks[i]];
            builder.sectorMark(side, track, sector.idSector);
        }

        const TrackByte &item = bytes[i];

        if(item.sync == 0xc2) {
            builder.syncC2();
            i++;
        } else if(item.sync != 0) {
            builder.syncA1();
            i++;
        } else if(item.mask != 0xff) {
            uint8_t pairs[2 * 256];
            int count = 0;

            do {
                pairs[2 * count]     = bytes[i].value;
                pairs[2 * count + 1] = bytes[i].mask;
                count++;
                i++;
            } while(i < trackEnd && count < 256 && bytes[i].mask != 0xff && bytes[i].sync == 0 && cellTimes[i] == 0 && sectorMarks[i] < 0);

            builder.fuzzyRun(pairs, count);
        } else {
            builder.data(item.value);
            i++;
        }
    }

    return true;
}
