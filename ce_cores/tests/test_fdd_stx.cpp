// floppyimagestx.cpp - STX images rebuilt into the track stream for the device. There's no real protected image in the
// tests: the files are made here to have just the wanted thing in them, by the format as Hatari reads it (floppies/stx.c).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "doctest.h"

#include "fdd/floppyimagest.h"
#include "fdd/floppyimagestx.h"
#include "fdd/mfmcachedimage.h"
#include "fdd/mfmtrackbuilder.h"
#include "fdd/imagesilo.h"

namespace {

typedef std::vector<uint8_t> Bytes;

void putU16(Bytes &b, int val) { b.push_back(val & 0xff); b.push_back((val >> 8) & 0xff); }
void putU32(Bytes &b, uint32_t val) { putU16(b, val & 0xffff); putU16(b, val >> 16); }

uint16_t floppyCrc(const Bytes &bytes)
{
    Bytes all = {0xa1, 0xa1, 0xa1};
    all.insert(all.end(), bytes.begin(), bytes.end());

    uint16_t crc = 0xffff;
    for(uint8_t val : all) {
        crc ^= ((uint16_t) val) << 8;
        for(int i=0; i<8; i++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

Bytes sectorData(uint8_t seed, int size = 512)
{
    Bytes data(size);
    uint32_t rnd = seed * 2654435761u + 1;
    for(int i=0; i<size; i++) {
        rnd = rnd * 1103515245 + 12345;
        data[i] = rnd >> 16;
    }
    return data;
}

struct StxSector {
    int bitPosition;
    int number;
    int status;
    Bytes data;
    Bytes fuzzy;
    Bytes timing;
    int readTime = 0;
    int idTrack = -1;
    int size = 2;
    int idCrc = -1;         // -1: the good one
};

struct StxTrack {
    int track = 0, side = 0;
    int mfmSize = 6250;
    int flags = 1;
    std::vector<StxSector> sectors;
    Bytes image;
    int imageSync = -1;     // >= 0: has a sync position
    Bytes rawSectors;       // for tracks without sector descriptors
};

Bytes makeStxFile(const std::vector<StxTrack> &tracks, int revision = 2)
{
    Bytes file = {'R', 'S', 'Y', 0};
    putU16(file, 3);
    putU16(file, 1);
    putU16(file, 0);
    file.push_back(tracks.size());
    file.push_back(revision);
    putU32(file, 0);

    for(const StxTrack &t : tracks) {
        Bytes fuzzy, data, timing;
        int flags = t.flags;

        if(!t.image.empty()) {
            flags |= 0x40;
            if(t.imageSync >= 0) {
                flags |= 0x80;
                putU16(data, t.imageSync);
            }
            putU16(data, t.image.size());
            data.insert(data.end(), t.image.begin(), t.image.end());
        }

        Bytes descriptors;
        for(const StxSector &s : t.sectors) {
            putU32(descriptors, data.size());
            putU16(descriptors, s.bitPosition);
            putU16(descriptors, s.readTime);

            uint8_t idTrack = (s.idTrack >= 0) ? s.idTrack : t.track;
            descriptors.push_back(idTrack);
            descriptors.push_back(t.side);
            descriptors.push_back(s.number);
            descriptors.push_back(s.size);

            uint16_t crc = (s.idCrc >= 0) ? s.idCrc : floppyCrc({0xfe, idTrack, (uint8_t) t.side, (uint8_t) s.number, (uint8_t) s.size});
            descriptors.push_back(crc >> 8);
            descriptors.push_back(crc & 0xff);
            descriptors.push_back(s.status);
            descriptors.push_back(0);

            data.insert(data.end(), s.data.begin(), s.data.end());
            fuzzy.insert(fuzzy.end(), s.fuzzy.begin(), s.fuzzy.end());
            timing.insert(timing.end(), s.timing.begin(), s.timing.end());
        }

        if(!timing.empty()) {
            putU16(data, 5);
            putU16(data, timing.size() + 4);
            data.insert(data.end(), timing.begin(), timing.end());
        }

        uint32_t blockSize = 16 + (t.rawSectors.empty() ? (descriptors.size() + fuzzy.size() + data.size()) : t.rawSectors.size());
        int count = t.rawSectors.empty() ? t.sectors.size() : t.rawSectors.size() / 512;

        putU32(file, blockSize);
        putU32(file, fuzzy.size());
        putU16(file, count);
        putU16(file, t.rawSectors.empty() ? flags : 0);
        putU16(file, t.mfmSize);
        file.push_back(t.track | (t.side << 7));
        file.push_back(0);

        if(t.rawSectors.empty()) {
            file.insert(file.end(), descriptors.begin(), descriptors.end());
            file.insert(file.end(), fuzzy.begin(), fuzzy.end());
            file.insert(file.end(), data.begin(), data.end());
        } else {
            file.insert(file.end(), t.rawSectors.begin(), t.rawSectors.end());
        }
    }

    return file;
}

// A byte of the stream, what the device makes from it.
struct Item {
    uint8_t value;
    uint8_t mask;
    uint8_t sync;
    int     cellTime;       // bit cell time in effect
    int     sector;         // from the last sector mark
};

// walks the stream as ce_pico_fdd/mfm_stream.cpp does
std::vector<Item> walkStream(const uint8_t *stream, int bufferSize)
{
    std::vector<Item> items;

    int end = (stream[0] << 8) | stream[1];
    REQUIRE(end <= bufferSize);
    REQUIRE(stream[2] == MFM_STREAM_VERSION);

    int cell = MFM_CELL_TIME_NOMINAL, sector = -1;
    int pos = MFM_STREAM_HEADER_SIZE;

    auto add = [&](uint8_t val, uint8_t mask, uint8_t sync) { Item i = {val, mask, sync, cell, sector}; items.push_back(i); };

    while(pos < end) {
        uint8_t val = stream[pos++];

        if(val != MFM_ESC) {
            add(val, 0xff, 0);
            continue;
        }

        uint8_t op = stream[pos++];

        switch(op) {
            case MFM_OP_LITERAL:    add(MFM_ESC, 0xff, 0); break;
            case MFM_OP_SYNC_A1:    add(0xa1, 0xff, 0xa1); break;
            case MFM_OP_SYNC_C2:    add(0xc2, 0xff, 0xc2); break;
            case MFM_OP_CELL_TIME:  cell = (stream[pos] << 8) | stream[pos + 1]; pos += 2; break;
            case MFM_OP_SECTOR:     sector = stream[pos + 2]; pos += 3; break;
            case MFM_OP_FUZZY: {
                int count = stream[pos++];
                count = count ? count : 256;
                for(int i=0; i<count; i++) {
                    add(stream[pos], stream[pos + 1], 0);
                    pos += 2;
                }
                break;
            }
            case MFM_OP_RAW: {
                int count = (stream[pos] << 8) | stream[pos + 1];
                pos += 2;
                for(int i=0; i<count; i++) {
                    add(stream[pos++], 0xff, 0);
                }
                break;
            }
            case MFM_OP_END:
                REQUIRE(pos == end);
                return items;
            default:
                FAIL("unknown opcode");
        }
    }

    FAIL("no end mark");
    return items;
}

struct StxFixture {
    std::string dir;

    StxFixture() {
        char tmpl[] = "/tmp/ce_test_stx_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
    }

    ~StxFixture() {
        unlink((dir + "/a.stx").c_str());
        unlink((dir + "/a.st").c_str());
        rmdir(dir.c_str());
    }

    std::string write(const Bytes &file, const char *name = "a.stx") {
        std::string path = dir + "/" + name;
        FILE *f = fopen(path.c_str(), "wb");
        REQUIRE(f != NULL);
        if(!file.empty()) {
            fwrite(file.data(), 1, file.size(), f);
        }
        fclose(f);
        return path;
    }

    std::vector<Item> build(FloppyImage &img, int track, int side) {
        std::vector<uint8_t> buffer(MFM_STREAM_SIZE);
        MfmTrackBuilder builder;
        builder.begin(buffer.data(), buffer.size());
        REQUIRE(img.buildTrack(builder, track, side));
        CHECK_FALSE(builder.overflowed());
        builder.finish();
        return walkStream(buffer.data(), buffer.size());
    }

    // the track of a file with this one track
    std::vector<Item> trackOf(const StxTrack &t, int revision = 2) {
        FloppyImageStx img;
        REQUIRE(img.open(write(makeStxFile({t}, revision)).c_str()));
        return build(img, t.track, t.side);
    }
};

// position of the ID field of this sector: the first byte after the 3x A1 + FE
int findId(const std::vector<Item> &items, int number)
{
    for(size_t i=3; i + 6 < items.size(); i++) {
        if(items[i].value == 0xfe && items[i - 1].sync == 0xa1 && items[i - 2].sync == 0xa1 && items[i - 3].sync == 0xa1 && items[i + 3].value == number) {
            return i + 1;
        }
    }
    return -1;
}

// position of the data mark after this position
int findDam(const std::vector<Item> &items, int from)
{
    for(size_t i=from; i + 1 < items.size(); i++) {
        if(items[i].sync == 0xa1 && items[i + 1].sync == 0 && items[i + 1].value >= 0xf8 && items[i + 1].value <= 0xfb) {
            return i + 1;
        }
    }
    return -1;
}

uint16_t crcAt(const std::vector<Item> &items, int pos)
{
    return (items[pos].value << 8) | items[pos + 1].value;
}

Bytes valuesAt(const std::vector<Item> &items, int pos, int count)
{
    Bytes out;
    for(int i=0; i<count; i++) {
        out.push_back(items[pos + i].value);
    }
    return out;
}

StxSector plainSector(int number, int status = 0)
{
    StxSector s;
    s.bitPosition = (60 + 16 + (number - 1) * 614) * 8;
    s.number = number;
    s.status = status;
    if((status & 0x10) == 0) {
        s.data = sectorData(number);
    }
    return s;
}

StxTrack plainTrack(int track, int side, int sectors = 9)
{
    StxTrack t;
    t.track = track;
    t.side = side;
    for(int i=1; i<=sectors; i++) {
        t.sectors.push_back(plainSector(i));
    }
    return t;
}

}   // namespace

TEST_SUITE("fdd stx image") {

TEST_CASE_FIXTURE(StxFixture, "geometry from the tracks, read only") {
    std::vector<StxTrack> tracks;
    for(int t=0; t<82; t++) {
        tracks.push_back(plainTrack(t, 0));
        tracks.push_back(plainTrack(t, 1));
    }
    tracks[5].sectors.pop_back();           // the most common count decides

    FloppyImageStx img;
    REQUIRE(img.open(write(makeStxFile(tracks)).c_str()));

    int t, s, p;
    REQUIRE(img.getParams(t, s, p));
    CHECK(t == 82);
    CHECK(s == 2);
    CHECK(p == 9);
    CHECK(img.isReadOnly());
    CHECK(img.isLoaded());

    uint8_t bfr[512];
    CHECK_FALSE(img.writeSector(0, 0, 1, bfr));
    CHECK_FALSE(img.readSector(0, 0, 1, bfr));
    CHECK_FALSE(img.gotUnsavedChanges());

    FloppyImageStx oneSide;
    REQUIRE(oneSide.open(write(makeStxFile({plainTrack(0, 0), plainTrack(1, 0)})).c_str()));
    REQUIRE(oneSide.getParams(t, s, p));
    CHECK(t == 2);
    CHECK(s == 1);
}

TEST_CASE_FIXTURE(StxFixture, "plain track gives the same bytes as the same sectors of .st image") {
    int tracks = 80, sides = 2, spt = 9;

    Bytes st;
    std::vector<StxTrack> stxTracks;
    for(int t=0; t<tracks; t++) {
        for(int s=0; s<sides; s++) {
            StxTrack track;
            track.track = t;
            track.side = s;
            for(int n=1; n<=spt; n++) {
                StxSector sector = plainSector(n);
                sector.data = sectorData(t * 20 + s * 10 + n);
                track.sectors.push_back(sector);
                st.insert(st.end(), sector.data.begin(), sector.data.end());
            }
            stxTracks.push_back(track);
        }
    }

    FloppyImageSt imgSt;
    REQUIRE(imgSt.open(write(st, "a.st").c_str()));
    FloppyImageStx imgStx;
    REQUIRE(imgStx.open(write(makeStxFile(stxTracks)).c_str()));

    MfmCachedImage *encoder = new MfmCachedImage();
    encoder->storeImageParams(&imgSt, SLOT_COUNT - 1);
    int t, s, guard = 0;
    while(encoder->findNotReadyTrackAndEncodeIt(&imgSt, t, s) && guard++ < 1000) {
    }

    for(auto &pos : std::vector<std::pair<int, int>>{{0, 0}, {40, 1}, {79, 1}}) {
        CAPTURE(pos.first);
        CAPTURE(pos.second);

        std::vector<uint8_t> buffer(MFM_STREAM_SIZE);
        int bytes = 0;
        REQUIRE(encoder->copyEncodedTrackIfReady(pos.first, pos.second, buffer.data(), bytes));
        std::vector<Item> expected = walkStream(buffer.data() + 2, buffer.size() - 2);
        std::vector<Item> got = build(imgStx, pos.first, pos.second);

        REQUIRE(got.size() == 6250);        // the rest of the track is just the gap
        REQUIRE(expected.size() == 60 + 9 * 614);

        int different = 0;
        for(size_t i=0; i<expected.size(); i++) {
            if(got[i].value != expected[i].value || got[i].sync != expected[i].sync ||
               got[i].mask != 0xff || got[i].cellTime != MFM_CELL_TIME_NOMINAL) {
                different++;
            }
        }
        CHECK(different == 0);

        for(size_t i=expected.size(); i<got.size(); i++) {
            CHECK(got[i].value == 0x4e);
        }
    }

    delete encoder;
}

TEST_CASE_FIXTURE(StxFixture, "the encoder uses the track of the image, and the device learns it's read only") {
    FloppyImageStx img;
    REQUIRE(img.open(write(makeStxFile({plainTrack(0, 0), plainTrack(0, 1), plainTrack(1, 0), plainTrack(1, 1)})).c_str()));

    MfmCachedImage *encoder = new MfmCachedImage();
    CHECK_FALSE(encoder->isReadOnly());
    encoder->storeImageParams(&img, SLOT_COUNT - 1);
    CHECK(encoder->isReadOnly());

    int t, s, guard = 0;
    while(encoder->findNotReadyTrackAndEncodeIt(&img, t, s) && guard++ < 100) {
    }

    std::vector<uint8_t> buffer(MFM_STREAM_SIZE);
    int bytes = 0;
    REQUIRE(encoder->copyEncodedTrackIfReady(1, 1, buffer.data(), bytes));
    CHECK(buffer[0] == 1);
    CHECK(buffer[1] == 1);

    std::vector<Item> items = walkStream(buffer.data() + 2, bytes - 2);
    CHECK(findId(items, 9) > 0);

    encoder->clearWholeCachedImage();
    CHECK_FALSE(encoder->isReadOnly());
    delete encoder;
}

TEST_CASE_FIXTURE(StxFixture, "sectors go to their positions, in the order of positions, with any numbers and sizes") {
    StxTrack t;
    StxSector a = plainSector(1), b = plainSector(2), c = plainSector(7);
    a.bitPosition = 3000 * 8;
    b.bitPosition = 200 * 8;                // first on the track, later in the file
    c.bitPosition = 1000 * 8;
    c.size = 0;                             // 128 B
    c.data = sectorData(7, 128);
    t.sectors = {a, b, c};

    std::vector<Item> items = trackOf(t);

    CHECK(findId(items, 2) == 200);         // the bit position is just after the ID mark
    CHECK(findId(items, 7) == 1000);
    CHECK(findId(items, 1) == 3000);

    int dam = findDam(items, 1000);
    CHECK(dam == 1000 + 6 + 22 + 12 + 3);
    CHECK(items[dam].value == 0xfb);
    CHECK(valuesAt(items, dam + 1, 128) == c.data);

    Bytes crcInput = {0xfb};
    crcInput.insert(crcInput.end(), c.data.begin(), c.data.end());
    CHECK(crcAt(items, dam + 129) == floppyCrc(crcInput));
    CHECK(items[1000 - 1].value == 0xfe);
    CHECK(crcAt(items, 1000 + 4) == floppyCrc({0xfe, 0, 0, 7, 0}));
    CHECK(items[1000 - 5].value == 0x00);   // gap before the sync
    CHECK(items[900].value == 0x4e);
}

TEST_CASE_FIXTURE(StxFixture, "sector numbers can repeat, and ID can be of other track") {
    StxTrack t;
    StxSector a = plainSector(3), b = plainSector(3);
    a.bitPosition = 200 * 8;
    b.bitPosition = 1000 * 8;
    b.idTrack = 40;
    b.data = sectorData(99);
    t.sectors = {a, b};

    std::vector<Item> items = trackOf(t);
    CHECK(items[200].value == 0);
    CHECK(items[1000].value == 40);
    CHECK(valuesAt(items, findDam(items, 1000) + 1, 512) == b.data);
}

TEST_CASE_FIXTURE(StxFixture, "CRC error in data: the CRC on the track is wrong") {
    StxTrack t;
    t.sectors = {plainSector(1, 0x08)};
    std::vector<Item> items = trackOf(t);

    int dam = findDam(items, 0);
    Bytes crcInput = {0xfb};
    crcInput.insert(crcInput.end(), t.sectors[0].data.begin(), t.sectors[0].data.end());
    CHECK(crcAt(items, dam + 513) != floppyCrc(crcInput));
    CHECK(valuesAt(items, dam + 1, 512) == t.sectors[0].data);
}

TEST_CASE_FIXTURE(StxFixture, "deleted data: other data mark, in the CRC too") {
    StxTrack t;
    t.sectors = {plainSector(1, 0x20)};
    std::vector<Item> items = trackOf(t);

    int dam = findDam(items, 0);
    CHECK(items[dam].value == 0xf8);

    Bytes crcInput = {0xf8};
    crcInput.insert(crcInput.end(), t.sectors[0].data.begin(), t.sectors[0].data.end());
    CHECK(crcAt(items, dam + 513) == floppyCrc(crcInput));
}

TEST_CASE_FIXTURE(StxFixture, "no data (Record Not Found): just the ID field, nothing after it") {
    StxTrack t;
    t.sectors = {plainSector(1, 0x10), plainSector(2)};
    std::vector<Item> items = trackOf(t);

    int id = findId(items, 1);
    REQUIRE(id > 0);
    CHECK(findDam(items, id) > findId(items, 2));   // the first data mark is the one of sector 2
    CHECK(crcAt(items, id + 4) == floppyCrc({0xfe, 0, 0, 1, 2}));
}

TEST_CASE_FIXTURE(StxFixture, "CRC error in ID field: stored CRC stays, a good one is made wrong") {
    StxTrack t;
    StxSector stored = plainSector(1, 0x18), good = plainSector(2, 0x18);
    stored.idCrc = 0x1234;
    t.sectors = {stored, good};
    std::vector<Item> items = trackOf(t);

    CHECK(crcAt(items, findId(items, 1) + 4) == 0x1234);
    CHECK(crcAt(items, findId(items, 2) + 4) != floppyCrc({0xfe, 0, 0, 2, 2}));
}

TEST_CASE_FIXTURE(StxFixture, "fuzzy bytes come with their masks, in the data only, sectors take the masks in order") {
    StxTrack t;
    StxSector a = plainSector(1, 0x88), b = plainSector(2), c = plainSector(3, 0x88);
    a.fuzzy = Bytes(512, 0xff);
    c.fuzzy = Bytes(512, 0xff);
    a.fuzzy[100] = 0x0f;
    for(int i=0; i<300; i++) {              // longer than one run in the stream
        c.fuzzy[i + 50] = i;
        if(i == 255) {
            c.fuzzy[i + 50] = 0x01;
        }
    }
    t.sectors = {a, b, c};
    std::vector<Item> items = trackOf(t);

    int damA = findDam(items, findId(items, 1));
    CHECK(items[damA + 1 + 100].mask == 0x0f);
    CHECK(items[damA + 1 + 100].value == a.data[100]);
    CHECK(items[damA + 1 + 99].mask == 0xff);
    CHECK(items[damA + 1 + 101].mask == 0xff);

    int damB = findDam(items, findId(items, 2));
    for(int i=0; i<512; i++) {
        CHECK(items[damB + 1 + i].mask == 0xff);
    }

    int damC = findDam(items, findId(items, 3));
    CHECK(valuesAt(items, damC + 1, 512) == c.data);
    int wrong = 0;
    for(int i=0; i<512; i++) {
        if(items[damC + 1 + i].mask != c.fuzzy[i]) {
            wrong++;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE_FIXTURE(StxFixture, "timing data: the bit cell time of each 16 bytes, standard again after the data") {
    StxTrack t;
    StxSector a = plainSector(1, 0x01), b = plainSector(2, 0x01);
    for(int block=0; block<32; block++) {
        a.timing.push_back(0);
        a.timing.push_back(block < 16 ? 0x85 : 0x79);
        b.timing.push_back(0);
        b.timing.push_back(0x7f);
    }
    t.sectors = {a, b};
    std::vector<Item> items = trackOf(t);

    int fast = (0x79 * 32 + 28) * 1000 / 128;       // 3900 cycles at 8 MHz for 16 bytes
    int slow = (0x85 * 32 + 28) * 1000 / 128;       // 4284
    CHECK(fast == 30468);
    CHECK(slow == 33468);

    int dam = findDam(items, findId(items, 1));
    CHECK(items[dam].cellTime == 32000);
    CHECK(items[dam + 1].cellTime == slow);
    CHECK(items[dam + 1 + 255].cellTime == slow);
    CHECK(items[dam + 1 + 256].cellTime == fast);
    CHECK(items[dam + 1 + 511].cellTime == fast);
    CHECK(items[dam + 513].cellTime == 32000);      // CRC

    int damB = findDam(items, findId(items, 2));
    CHECK(items[damB + 1].cellTime == (0x7f * 32 + 28) * 1000 / 128);   // the table of the second sector
}

TEST_CASE_FIXTURE(StxFixture, "old images (revision 0) have no timing data, the table of Pasti is used") {
    StxTrack t;
    t.sectors = {plainSector(1, 0x01)};
    std::vector<Item> items = trackOf(t, 0);

    int dam = findDam(items, 0);
    CHECK(items[dam + 1].cellTime == (0x7f * 32 + 28) * 1000 / 128);
    CHECK(items[dam + 1 + 128].cellTime == (0x85 * 32 + 28) * 1000 / 128);
    CHECK(items[dam + 1 + 256].cellTime == (0x79 * 32 + 28) * 1000 / 128);
    CHECK(items[dam + 1 + 384].cellTime == (0x7f * 32 + 28) * 1000 / 128);
}

TEST_CASE_FIXTURE(StxFixture, "read time of a sector without timing data: its bit cell time") {
    StxTrack t;
    t.sectors = {plainSector(1)};
    t.sectors[0].readTime = 15360;          // 30 us per byte instead of 32
    std::vector<Item> items = trackOf(t);

    int dam = findDam(items, 0);
    CHECK(items[dam + 1].cellTime == 30000);
    CHECK(items[dam + 513].cellTime == 32000);
}

TEST_CASE_FIXTURE(StxFixture, "long or short track: bit rate changed so it fits a rotation, sane sizes only") {
    auto firstCell = [&](int mfmSize) {
        StxTrack t = plainTrack(0, 0, 1);
        t.mfmSize = mfmSize;
        return trackOf(t)[0].cellTime;
    };

    CHECK(firstCell(6250) == 32000);
    CHECK(firstCell(6400) == 31250);
    CHECK(firstCell(6100) == 200000000 / 6100);
    CHECK(firstCell(9000) == 200000000 / 6800);
    CHECK(firstCell(100) == 200000000 / 5800);

    StxTrack t = plainTrack(0, 0, 1);
    t.mfmSize = 6400;
    CHECK(trackOf(t).size() == 6400);
}

TEST_CASE_FIXTURE(StxFixture, "track without sector descriptors: just the sectors, standard layout") {
    StxTrack t;
    t.track = 3;
    t.side = 1;
    t.mfmSize = 6250 * 8;                   // in bits here
    for(int n=1; n<=9; n++) {
        Bytes d = sectorData(n);
        t.rawSectors.insert(t.rawSectors.end(), d.begin(), d.end());
    }
    std::vector<Item> items = trackOf(t);

    CHECK(items.size() == 6250);
    for(int n=1; n<=9; n++) {
        CAPTURE(n);
        int id = findId(items, n);
        REQUIRE(id > 0);
        CHECK(items[id].value == 3);
        CHECK(items[id + 1].value == 1);
        CHECK(crcAt(items, id + 4) == floppyCrc({0xfe, 3, 1, (uint8_t) n, 2}));
        CHECK(valuesAt(items, findDam(items, id) + 1, 512) == sectorData(n));
    }
}

TEST_CASE_FIXTURE(StxFixture, "not formatted track, or a track not in the file: empty, no ID field to find") {
    StxTrack t;
    t.track = 2;
    FloppyImageStx img;
    REQUIRE(img.open(write(makeStxFile({t, plainTrack(5, 0)})).c_str()));

    CHECK(build(img, 2, 0).empty());
    CHECK(build(img, 3, 0).empty());
    CHECK(build(img, 5, 1).empty());
    CHECK(build(img, 84, 0).empty());
    CHECK(build(img, 85, 0).empty());
    CHECK(build(img, -1, 0).empty());
    CHECK_FALSE(build(img, 5, 0).empty());
}

TEST_CASE_FIXTURE(StxFixture, "track image: its bytes are used, the marks found in it have missing clock") {
    StxTrack t;
    t.image = Bytes(6300, 0x4e);
    t.image[10] = 0x5a;                     // gaps with anything in them stay

    auto putMark = [&](int pos, uint8_t val, uint8_t mark) {
        t.image[pos] = t.image[pos + 1] = t.image[pos + 2] = val;
        t.image[pos + 3] = mark;
    };
    putMark(20, 0xc2, 0xfc);                // index mark
    putMark(100, 0xa1, 0xfe);               // ID field of something not in the sectors
    t.image[300] = 0xa1;                    // A1 in data without a mark after it isn't a sync
    t.image[301] = 0xa1;
    t.image[302] = 0xa1;
    t.image[303] = 0x77;

    std::vector<Item> items = trackOf(t);

    REQUIRE(items.size() == 6300);
    CHECK(items[0].cellTime == 200000000 / 6300);   // the track image knows the length
    CHECK(items[10].value == 0x5a);
    CHECK(items[20].sync == 0xc2);
    CHECK(items[22].sync == 0xc2);
    CHECK(items[23].sync == 0);
    CHECK(items[23].value == 0xfc);
    CHECK(items[100].sync == 0xa1);
    CHECK(items[103].sync == 0);
    CHECK(items[300].sync == 0);
    CHECK(items[300].value == 0xa1);
}

// Read Track of a real floppy, as it's in the real images: 00 00 14 A1 A1 FE - the first byte of the sync mark is garbled
void putRealMark(Bytes &image, int pos, uint8_t mark)
{
    image[pos] = 0x14;
    image[pos + 1] = image[pos + 2] = 0xa1;
    image[pos + 3] = mark;
}

TEST_CASE_FIXTURE(StxFixture, "track image of a real floppy: the garbled first byte of the sync mark is a sync mark too") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);
    putRealMark(t.image, 100, 0xfe);        // ID field of something not in the sectors
    putRealMark(t.image, 200, 0xfb);
    t.image[300] = 0x14;                    // without the mark after it: data
    t.image[301] = t.image[302] = 0xa1;
    t.image[303] = 0x77;

    std::vector<Item> items = trackOf(t);

    CHECK(items[100].sync == 0xa1);
    CHECK(items[100].value == 0xa1);
    CHECK(items[102].sync == 0xa1);
    CHECK(items[103].sync == 0);
    CHECK(items[103].value == 0xfe);
    CHECK(items[200].sync == 0xa1);
    CHECK(items[300].sync == 0);
    CHECK(items[300].value == 0x14);
    CHECK(items[301].sync == 0);
}

TEST_CASE_FIXTURE(StxFixture, "track image of a real floppy: data field found by its garbled mark, also at other than standard distance") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);

    const int idMark = 500;
    putRealMark(t.image, idMark - 3, 0xfe);
    t.image[idMark + 1] = 0;
    t.image[idMark + 2] = 0;
    t.image[idMark + 3] = 4;
    t.image[idMark + 4] = 2;

    const int syncPos = idMark + 7 + 25;
    putRealMark(t.image, syncPos, 0xfb);

    StxSector s = plainSector(4);
    s.bitPosition = (idMark + 1) * 8;
    t.sectors = {s};

    std::vector<Item> items = trackOf(t);

    CHECK(findId(items, 4) == idMark + 1);
    CHECK(findDam(items, idMark) == syncPos + 3);
    CHECK(valuesAt(items, syncPos + 4, 512) == s.data);
}

TEST_CASE_FIXTURE(StxFixture, "track image longer than standard: ID far from its bit position is found, the sector isn't there twice") {
    StxTrack t;
    t.image = Bytes(6315, 0x4e);

    const int idMark = 5650;                // the bit position says 5600: 50 bytes off, at the end of a long track
    putRealMark(t.image, idMark - 3, 0xfe);
    t.image[idMark + 1] = 0;
    t.image[idMark + 2] = 0;
    t.image[idMark + 3] = 10;
    t.image[idMark + 4] = 2;

    StxSector s = plainSector(10);
    s.bitPosition = (5600 + 1) * 8;
    t.sectors = {s};

    std::vector<Item> items = trackOf(t);

    CHECK(findId(items, 10) == idMark + 1);

    int ids = 0;
    for(size_t i=1; i<items.size(); i++) {
        ids += (items[i].value == 0xfe && items[i - 1].sync == 0xa1) ? 1 : 0;
    }
    CHECK(ids == 1);
}

TEST_CASE_FIXTURE(StxFixture, "ID field garbled in the track image: put over the mark found close, not as a second one elsewhere") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);

    const int idMark = 1000;
    putRealMark(t.image, idMark - 3, 0xfe);
    t.image[idMark + 1] = 0x55;             // garbage instead of the track number
    t.image[idMark + 2] = 0;
    t.image[idMark + 3] = 4;
    t.image[idMark + 4] = 2;

    StxSector s = plainSector(4);
    s.bitPosition = (idMark + 10 + 1) * 8;
    t.sectors = {s};

    std::vector<Item> items = trackOf(t);

    CHECK(findId(items, 4) == idMark + 1);
    CHECK(items[idMark + 1].value == 0);    // from the sector
    CHECK(crcAt(items, idMark + 5) == floppyCrc({0xfe, 0, 0, 4, 2}));
}

TEST_CASE_FIXTURE(StxFixture, "same ID field many times on the track: each sector goes to the closest one") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);

    for(int idMark : {500, 2500}) {
        putRealMark(t.image, idMark - 3, 0xfe);
        t.image[idMark + 1] = 0;
        t.image[idMark + 2] = 0;
        t.image[idMark + 3] = 3;
        t.image[idMark + 4] = 2;
    }

    StxSector first = plainSector(3), second = plainSector(3);
    first.bitPosition = 501 * 8;
    second.bitPosition = 2501 * 8;
    second.data = sectorData(44);
    t.sectors = {first, second};

    std::vector<Item> items = trackOf(t);

    CHECK(valuesAt(items, findDam(items, 500) + 1, 512) == first.data);
    CHECK(valuesAt(items, findDam(items, 2500) + 1, 512) == second.data);
}

TEST_CASE_FIXTURE(StxFixture, "track image with sync position: the same, the position doesn't move anything") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);
    t.image[5] = 0x11;
    t.imageSync = 1234;

    std::vector<Item> items = trackOf(t);
    CHECK(items.size() == 6250);
    CHECK(items[5].value == 0x11);
}

TEST_CASE_FIXTURE(StxFixture, "track image and sectors: sector goes where its ID is in the image, data from the sector (Read Track can return garbage there)") {
    StxTrack t;
    t.image = Bytes(6250, 0x4e);

    const int idMark = 500;                 // the file says +6 B
    t.image[idMark - 3] = t.image[idMark - 2] = t.image[idMark - 1] = 0xa1;
    t.image[idMark] = 0xfe;
    t.image[idMark + 1] = 0;
    t.image[idMark + 2] = 0;
    t.image[idMark + 3] = 4;
    t.image[idMark + 4] = 2;

    const int syncPos = idMark + 7 + 25;    // not the standard distance
    t.image[syncPos] = t.image[syncPos + 1] = t.image[syncPos + 2] = 0xa1;
    t.image[syncPos + 3] = 0xfb;
    t.image[syncPos + 10] = 0x99;           // garbage from Read Track

    StxSector s = plainSector(4);
    s.bitPosition = (idMark + 1 + 6) * 8;
    t.sectors = {s};

    std::vector<Item> items = trackOf(t);

    CHECK(findId(items, 4) == idMark + 1);
    CHECK(findDam(items, idMark) == syncPos + 3);
    CHECK(valuesAt(items, syncPos + 4, 512) == s.data);
    CHECK(crcAt(items, idMark + 5) == floppyCrc({0xfe, 0, 0, 4, 2}));
}

TEST_CASE_FIXTURE(StxFixture, "sector in the data of another one: its marks stay in the data, the gaps don't overwrite the data") {
    StxTrack t;
    StxSector outer = plainSector(1), inner = plainSector(2);
    outer.bitPosition = 100 * 8;            // data from about 150 to 660
    inner.bitPosition = 300 * 8;            // its ID in the data, its data after the outer one
    outer.data = Bytes(512, 0x33);
    t.sectors = {outer, inner};

    std::vector<Item> items = trackOf(t);

    int idInner = findId(items, 2);
    REQUIRE(idInner == 300);
    CHECK(items[299 - 3].sync == 0xa1);
    CHECK(items[299 - 4].value == 0x33);    // the gap before the sync of the inner one didn't overwrite the data of the outer
    CHECK(findId(items, 1) == 100);
}

TEST_CASE_FIXTURE(StxFixture, "what isn't an STX image is refused: other file with the same extension, bad version, nothing") {
    Bytes good = makeStxFile({plainTrack(0, 0)});

    Bytes sting = {0x60, 0x1a, 0, 0, 0x10, 0, 0, 0};    // STinG module: a program
    sting.resize(5000, 0x4e);
    Bytes version = good;
    version[4] = 2;
    Bytes noTracks = makeStxFile({});

    for(const Bytes &file : std::vector<Bytes>{sting, version, noTracks, Bytes(), Bytes(10, 'R')}) {
        FloppyImageStx img;
        CHECK_FALSE(img.open(write(file).c_str()));
        CHECK_FALSE(img.isLoaded());
    }

    FloppyImageStx img;
    CHECK_FALSE(img.open((dir + "/missing.stx").c_str()));
}

TEST_CASE_FIXTURE(StxFixture, "damaged files: refused or read, but never outside of the file") {
    StxTrack t = plainTrack(1, 1, 2);
    t.sectors[0].status = 0x89;
    t.sectors[0].fuzzy = Bytes(512, 0xf0);
    for(int i=0; i<64; i++) {
        t.sectors[0].timing.push_back(0x7f);
    }
    t.image = Bytes(100, 0xa1);
    Bytes good = makeStxFile({t, plainTrack(2, 0, 1)});

    auto tryFile = [&](const Bytes &file) {
        FloppyImageStx img;
        if(!img.open(write(file).c_str())) {
            return;
        }
        for(int track=0; track<3; track++) {
            for(int side=0; side<2; side++) {
                std::vector<uint8_t> buffer(MFM_STREAM_SIZE);
                MfmTrackBuilder builder;
                builder.begin(buffer.data(), buffer.size());
                CHECK(img.buildTrack(builder, track, side));
                builder.finish();
            }
        }
    };

    for(size_t size=0; size<good.size(); size += (size < 200) ? 1 : 97) {    // cut anywhere
        tryFile(Bytes(good.begin(), good.begin() + size));
    }

    uint32_t rnd = 12345;
    for(int i=0; i<300; i++) {              // wrong values in the headers
        Bytes bad = good;
        for(int j=0; j<4; j++) {
            rnd = rnd * 1103515245 + 12345;
            int pos = (rnd >> 8) % 120;
            rnd = rnd * 1103515245 + 12345;
            bad[pos] = (rnd >> 16) & 1 ? 0xff : (rnd >> 8);
        }
        tryFile(bad);
    }
}

TEST_CASE_FIXTURE(StxFixture, "worst case track still fits the stream: all sectors with fuzzy bytes and timing") {
    StxTrack t;
    for(int n=1; n<=10; n++) {
        StxSector s = plainSector(n, 0x89);
        s.fuzzy = Bytes(512, 0xf0);
        for(int block=0; block<32; block++) {
            s.timing.push_back(0);
            s.timing.push_back(0x7f + (block % 3));
        }
        t.sectors.push_back(s);
    }

    std::vector<Item> items = trackOf(t);       // no overflow is checked inside
    CHECK(findId(items, 10) > 0);
}

}   // TEST_SUITE
