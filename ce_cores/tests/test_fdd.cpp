/*
    Tests of the floppy (fdd) code - ST / MSA images, MSA compression, MFM encoding / decoding and the floppy encoder thread
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/utils.h"
#include "fdd/floppyimagest.h"
#include "fdd/floppyimagemsa.h"
#include "fdd/msa.h"
#include "fdd/mfmcachedimage.h"
#include "fdd/imagesilo.h"
#include "fdd/floppyencoder.h"

extern SiloSlot slots[SLOT_COUNT];                      // floppy encoder globals, defined in floppyencoder.cpp
extern volatile uint8_t slotSendToDevice[SLOT_COUNT];
extern pthread_mutex_t floppyEncoderMutex;

//----------------------------------------------------------------------
// helpers

// creates empty temp dir, removes it at the end of test
struct FddTmpDir {
    std::string dir;

    FddTmpDir() {
        char tmpl[] = "/tmp/ce_test_fdd_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
    }

    ~FddTmpDir() {
        std::string cmd = "chmod -R u+w '" + dir + "' && rm -rf '" + dir + "'";
        system(cmd.c_str());
    }

    std::string path(const char *name) { return dir + "/" + name; }

    int countTempFiles(void) {          // leftovers of saveFileAtomically()
        int cnt = 0;
        DIR *d = opendir(dir.c_str());
        struct dirent *e;
        while((e = readdir(d)) != NULL) {
            if(strstr(e->d_name, ".tmp-")) {
                cnt++;
            }
        }
        closedir(d);
        return cnt;
    }
};

static int sectorIndex(int track, int side, int sector, int sides, int spt)    // index of sector in a raw image (sector is 1-based)
{
    return (track * sides + side) * spt + (sector - 1);
}

// image data where every sector has its own content, so moved / swapped sectors are detected
static std::vector<uint8_t> makeImageData(int tracks, int sides, int spt, bool random)
{
    std::vector<uint8_t> data(tracks * sides * spt * 512);
    srand(tracks * 1000 + sides * 100 + spt);

    for(size_t i=0; i<data.size(); i++) {
        data[i] = random ? (uint8_t) rand() : (uint8_t) ((i / 512) * 7 + (i % 512 < 256 ? 0 : 0xE5));
    }

    return data;
}

static void writeFile(const std::string &path, const std::vector<uint8_t> &data)
{
    FILE *f = fopen(path.c_str(), "wb");
    REQUIRE(f != NULL);
    REQUIRE(fwrite(data.data(), 1, data.size(), f) == data.size());
    fclose(f);
}

static std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) {
        return data;
    }
    fseek(f, 0, SEEK_END);
    data.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(got);
    return data;
}

static bool sectorIs(FloppyImage *img, int track, int side, int sector, const uint8_t *expected)
{
    uint8_t buf[512];
    return img->readSector(track, side, sector, buf) && memcmp(buf, expected, 512) == 0;
}

static int msaStartTrack(const std::string &path)       // StartingTrack word from MSA header
{
    std::vector<uint8_t> d = readFile(path);
    REQUIRE(d.size() >= 10);
    return (d[6] << 8) | d[7];
}

// decode MSA file using a buffer of exactly its size (so a read beyond it would be caught by ASan, if enabled)
static bool msaDecode(const std::vector<uint8_t> &file, size_t len, std::vector<uint8_t> &out)
{
    uint8_t *copy = (uint8_t *) malloc(len ? len : 1);
    memcpy(copy, file.data(), len);

    long size = 0;
    uint8_t *res = MSA_UnCompress(copy, (long) len, &size);
    free(copy);

    if(!res) {
        return false;
    }

    out.assign(res, res + size);
    free(res);
    return true;
}

static void encodeWholeImage(MfmCachedImage *enc, FloppyImage *img)
{
    int track, side;
    int guard = 0;
    while(enc->findNotReadyTrackAndEncodeIt(img, track, side) && guard++ < 1000) {
    }
}

// Walks the v2 stream of a track (as ce_pico_fdd/mfm_stream.cpp does) to find the raw run of a sector.
static bool findSectorRun(const uint8_t *stream, int streamSize, int sector, int &runStart, int &runSize)
{
    int end = (stream[0] << 8) | stream[1];
    int pos = MFM_STREAM_HEADER_SIZE;
    int lastSector = -1;

    if(end > streamSize || stream[2] != MFM_STREAM_VERSION) {
        return false;
    }

    while(pos < end) {
        if(stream[pos++] != MFM_ESC) {
            continue;
        }

        switch(stream[pos++]) {
            case MFM_OP_LITERAL:
            case MFM_OP_SYNC_A1:
            case MFM_OP_SYNC_C2:
                break;

            case MFM_OP_SECTOR:
                lastSector = stream[pos + 2];
                pos += 3;
                break;

            case MFM_OP_RAW:
                runSize = (stream[pos] << 8) | stream[pos + 1];
                pos += 2;

                if(lastSector == sector) {
                    runStart = pos;
                    return true;
                }

                pos += runSize;
                break;

            default:
                return false;
        }
    }

    return false;
}

// What the device captures when the ST writes a sector: 2-bit interval symbols, 4 in a byte. Made the way the v1 encoder
// (and the hardware) did it, as the decoder on the host still gets the captured writes in this form.
struct SymbolEncoder {
    uint8_t threeBits = 1, times = 0, timesCnt = 0;
    std::vector<uint8_t> out;

    void symbol(uint8_t time) {
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
                case 0: case 3: case 7: symbol(1); break;   // 4 us
                case 1: case 4:         symbol(2); break;   // 6 us
                case 5:                 symbol(3); break;   // 8 us
            }
        }
    }

    void syncA1(void) {
        symbol((threeBits == 0 || threeBits == 4) ? 2 : ((threeBits == 2 || threeBits == 6) ? 3 : 1));
        symbol(3);
        symbol(2);
        symbol(3);
        symbol(2);
        threeBits = 1;
    }
};

// Get the data part of one encoded sector, as the device would send it on sector write: 12x 00, 3x A1, raw run, gap.
static bool tryGetSectorDataPartMfm(MfmCachedImage *enc, int track, int side, int sector, std::vector<uint8_t> &mfm)
{
    std::vector<uint8_t> trackBuf(MFM_STREAM_SIZE);
    int bytes = 0;
    if(!enc->copyEncodedTrackIfReady(track, side, trackBuf.data(), bytes)) {    // copy is taken under mutex - safe while encoder runs
        return false;
    }

    int runStart = 0, runSize = 0;
    if(!findSectorRun(trackBuf.data() + MFM_STREAM_PREFIX_SIZE, bytes - MFM_STREAM_PREFIX_SIZE, sector, runStart, runSize) || runSize != MFM_SECTOR_RUN_SIZE) {
        return false;
    }

    SymbolEncoder symbols;
    for(int i=0; i<12; i++) {
        symbols.byte(0);
    }
    for(int i=0; i<3; i++) {
        symbols.syncA1();
    }
    for(int i=0; i<runSize; i++) {
        symbols.byte(trackBuf[MFM_STREAM_PREFIX_SIZE + runStart + i]);
    }
    for(int i=0; i<40; i++) {
        symbols.byte(0x4e);
    }

    mfm = symbols.out;
    return true;
}

static std::vector<uint8_t> getSectorDataPartMfm(MfmCachedImage *enc, int track, int side, int sector)
{
    std::vector<uint8_t> mfm;
    REQUIRE(tryGetSectorDataPartMfm(enc, track, side, sector, mfm));
    return mfm;
}

static bool decodeSector(MfmCachedImage *enc, const std::vector<uint8_t> &mfm, uint8_t *out512)
{
    std::vector<uint8_t> copy(mfm);         // decoder shifts the input in place and may look 1 byte past the end
    copy.resize(copy.size() + 16, 0);
    return enc->decodeMfmBuffer(copy.data(), (int) mfm.size(), out512);
}

//----------------------------------------------------------------------
// ST images

TEST_CASE_FIXTURE(FddTmpDir, "st image: geometry is detected from size, unknown size is refused")
{
    writeFile(path("std.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt good;
    REQUIRE(good.open(path("std.st").c_str()));
    int t, s, p;
    REQUIRE(good.getParams(t, s, p));
    CHECK(t == 80);
    CHECK(s == 2);
    CHECK(p == 9);

    struct { int tracks, sides, spt; } sizes[] = {
        {80, 1, 8}, {80, 1, 9}, {80, 1, 10}, {80, 1, 11}, {80, 2, 8}, {80, 2, 10}, {80, 2, 11}, {82, 1, 10}, {83, 2, 9}
    };
    for(auto &g : sizes) {
        CAPTURE(g.tracks);
        CAPTURE(g.sides);
        CAPTURE(g.spt);
        writeFile(path("g.st"), makeImageData(g.tracks, g.sides, g.spt, false));
        FloppyImageSt img;
        REQUIRE(img.open(path("g.st").c_str()));
        REQUIRE(img.getParams(t, s, p));
        CHECK(t == g.tracks);
        CHECK(s == g.sides);
        CHECK(p == g.spt);
    }

    std::vector<uint8_t> odd(1000 * 512 + 100);         // matches no tracks * sides * spt combination
    writeFile(path("odd.st"), odd);
    FloppyImageSt bad;
    CHECK_FALSE(bad.open(path("odd.st").c_str()));
    CHECK_FALSE(bad.isLoaded());
}

TEST_CASE_FIXTURE(FddTmpDir, "st image: writes aren't saved over a file replaced on disk, otherwise saving goes on as usual")
{
    std::vector<uint8_t> original(80 * 2 * 9 * 512, 0x11);
    std::vector<uint8_t> replaced(80 * 2 * 9 * 512, 0x22);
    std::vector<uint8_t> sector(512, 0x33);

    writeFile(path("a.st"), original);
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));
    CHECK_FALSE(img.fileChangedOnDisk());

    img.writeSector(0, 0, 1, sector.data());                // own saves don't count as a change, also more of them
    CHECK(img.save());
    CHECK_FALSE(img.fileChangedOnDisk());
    img.writeSector(0, 0, 2, sector.data());
    CHECK(img.save());
    CHECK(readFile(path("a.st"))[512] == 0x33);

    writeFile(path("new.st"), replaced);                    // the same size in the same second - it's another file
    REQUIRE(rename(path("new.st").c_str(), path("a.st").c_str()) == 0);
    CHECK(img.fileChangedOnDisk());

    img.writeSector(0, 0, 3, sector.data());
    CHECK(img.gotUnsavedChanges());
    CHECK_FALSE(img.save());
    CHECK_FALSE(img.gotUnsavedChanges());                   // not trying again and again
    CHECK(readFile(path("a.st")) == replaced);

    REQUIRE(remove(path("a.st").c_str()) == 0);             // deleted: nothing to lose, saved again
    CHECK_FALSE(img.fileChangedOnDisk());
    img.writeSector(0, 0, 3, sector.data());
    CHECK(img.save());
    CHECK(readFile(path("a.st")).size() == original.size());
}

TEST_CASE_FIXTURE(FddTmpDir, "msa image: writes aren't saved over a file replaced on disk")
{
    std::vector<uint8_t> data = makeImageData(80, 2, 9, false);
    std::vector<uint8_t> sector(512, 0x33);
    REQUIRE(MSA_WriteDisk(path("a.msa").c_str(), data.data(), (int) data.size(), 80, 2, 9));

    FloppyImageMsa img;
    REQUIRE(img.open(path("a.msa").c_str()));
    img.writeSector(0, 0, 1, sector.data());
    CHECK(img.save());
    CHECK_FALSE(img.fileChangedOnDisk());

    std::vector<uint8_t> other = {1, 2, 3};
    writeFile(path("new.msa"), other);
    REQUIRE(rename(path("new.msa").c_str(), path("a.msa").c_str()) == 0);

    img.writeSector(0, 0, 2, sector.data());
    CHECK_FALSE(img.save());
    CHECK(readFile(path("a.msa")) == other);
}

TEST_CASE_FIXTURE(FddTmpDir, "st image: reads and writes outside of the geometry are refused")
{
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    uint8_t buf[512];
    memset(buf, 0x11, 512);
    CHECK(img.writeSector(79, 1, 9, buf));          // last valid sector
    CHECK_FALSE(img.writeSector(80, 0, 1, buf));    // track beyond image (device can send track 0..127)
    CHECK_FALSE(img.writeSector(127, 1, 1, buf));
    CHECK_FALSE(img.writeSector(0, 2, 1, buf));     // side 2
    CHECK_FALSE(img.writeSector(-1, 0, 1, buf));
    CHECK_FALSE(img.writeSector(0, 0, 0, buf));     // sectors are 1-based
    CHECK_FALSE(img.writeSector(0, 0, 10, buf));
    CHECK_FALSE(img.readSector(80, 0, 1, buf));
}

TEST_CASE_FIXTURE(FddTmpDir, "st image: deleting image with unsaved writes saves them")
{
    // used to abort with 'pure virtual method called' - base destructor called save()
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));

    uint8_t mark[512];
    memset(mark, 0xA5, 512);

    FloppyImage *img = new FloppyImageSt();
    REQUIRE(img->open(path("a.st").c_str()));
    REQUIRE(img->writeSector(10, 1, 5, mark));
    CHECK(img->gotUnsavedChanges());
    delete img;

    std::vector<uint8_t> saved = readFile(path("a.st"));
    REQUIRE(saved.size() == 80 * 2 * 9 * 512);
    CHECK(memcmp(saved.data() + sectorIndex(10, 1, 5, 2, 9) * 512, mark, 512) == 0);
}

TEST_CASE_FIXTURE(FddTmpDir, "st image: save is atomic - keeps permissions and symlink, leaves no temp file")
{
    writeFile(path("disk.st"), makeImageData(80, 2, 9, false));
    REQUIRE(chmod(path("disk.st").c_str(), 0640) == 0);
    REQUIRE(symlink(path("disk.st").c_str(), path("link.st").c_str()) == 0);

    uint8_t mark[512];
    memset(mark, 0x77, 512);

    FloppyImageSt img;
    REQUIRE(img.open(path("link.st").c_str()));
    REQUIRE(img.writeSector(0, 0, 1, mark));
    CHECK(img.save());

    struct stat st;
    REQUIRE(lstat(path("link.st").c_str(), &st) == 0);
    CHECK(S_ISLNK(st.st_mode));                         // symlink not replaced by a file

    REQUIRE(stat(path("disk.st").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);                 // permissions of original kept

    std::vector<uint8_t> saved = readFile(path("disk.st"));
    REQUIRE(saved.size() >= 512);
    CHECK(memcmp(saved.data(), mark, 512) == 0);        // data went to the symlink target

    CHECK(countTempFiles() == 0);
}

TEST_CASE_FIXTURE(FddTmpDir, "st image: failed save leaves the original file intact")
{
    std::string sub = path("ro");
    REQUIRE(mkdir(sub.c_str(), 0755) == 0);

    // root can write to read-only dirs, and so can an administrator under Cygwin (permissions there are only mapped
    // to Windows ACLs) - can't test this then
    REQUIRE(chmod(sub.c_str(), 0555) == 0);
    std::string probe = sub + "/probe";
    int probeFd = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    REQUIRE(chmod(sub.c_str(), 0755) == 0);
    if(probeFd >= 0) {
        close(probeFd);
        unlink(probe.c_str());
        MESSAGE("read-only dir is writable here (root / Windows administrator), skipping");
        return;
    }

    std::string file = sub + "/x.st";
    std::vector<uint8_t> original = makeImageData(80, 2, 9, false);
    writeFile(file, original);

    FloppyImage *img = new FloppyImageSt();
    REQUIRE(img->open(file.c_str()));
    REQUIRE(chmod(sub.c_str(), 0555) == 0);             // can't create the temp file now

    uint8_t mark[512];
    memset(mark, 0x77, 512);
    REQUIRE(img->writeSector(0, 0, 1, mark));
    CHECK_FALSE(img->save());
    delete img;                                         // destructor tries to save again - fails too

    REQUIRE(chmod(sub.c_str(), 0755) == 0);
    CHECK(readFile(file) == original);
}

//----------------------------------------------------------------------
// MSA

TEST_CASE_FIXTURE(FddTmpDir, "msa: written file decodes back to the same data")
{
    for(int random = 0; random <= 1; random++) {        // compressible data with E5 bytes, then incompressible random data
        CAPTURE(random);
        std::vector<uint8_t> img = makeImageData(80, 2, 9, random);
        REQUIRE(MSA_WriteDisk(path("a.msa").c_str(), img.data(), (int) img.size(), 80, 2, 9));

        std::vector<uint8_t> file = readFile(path("a.msa"));

        // walk track blocks: every length word must be valid and the blocks must end exactly at the end of file
        size_t pos = 10;
        int blocks = 0, badLengths = 0;
        while(pos + 2 <= file.size()) {
            int len = (file[pos] << 8) | file[pos + 1];
            if(len < 1 || len > 9 * 512) {
                badLengths++;
                break;
            }
            pos += 2 + len;
            blocks++;
        }
        CHECK(badLengths == 0);
        CHECK(blocks == 160);
        CHECK(pos == file.size());

        std::vector<uint8_t> decoded;
        REQUIRE(msaDecode(file, file.size(), decoded));
        CHECK(decoded == img);
    }
}

TEST_CASE_FIXTURE(FddTmpDir, "msa: truncated files and invalid headers are refused")
{
    std::vector<uint8_t> img = makeImageData(80, 2, 9, false);
    REQUIRE(MSA_WriteDisk(path("a.msa").c_str(), img.data(), (int) img.size(), 80, 2, 9));
    std::vector<uint8_t> file = readFile(path("a.msa"));

    std::vector<uint8_t> out;
    int accepted = 0, tried = 0;
    for(size_t len = 0; len < file.size(); len += (len < 2048 ? 1 : 997)) {
        accepted += msaDecode(file, len, out) ? 1 : 0;
        tried++;
    }
    CHECK(tried > 2000);
    CHECK(accepted == 0);

    std::vector<uint8_t> badId = {0x0e, 0x10, 0, 9, 0, 1, 0, 0, 0, 79};
    std::vector<uint8_t> startAfterEnd = {0x0e, 0x0f, 0, 9, 0, 1, 0, 80, 0, 10};
    std::vector<uint8_t> hugeSpt = {0x0e, 0x0f, 0xff, 0xff, 0, 1, 0, 0, 0xff, 0xff};
    std::vector<uint8_t> threeSides = {0x0e, 0x0f, 0, 9, 0, 2, 0, 0, 0, 79};
    CHECK_FALSE(msaDecode(badId, badId.size(), out));
    CHECK_FALSE(msaDecode(startAfterEnd, startAfterEnd.size(), out));
    CHECK_FALSE(msaDecode(hugeSpt, hugeSpt.size(), out));
    CHECK_FALSE(msaDecode(threeSides, threeSides.size(), out));

    writeFile(path("short.msa"), std::vector<uint8_t>{0x0e, 0x0f, 0, 9});    // header cut off
    FloppyImageMsa msa;
    CHECK_FALSE(msa.open(path("short.msa").c_str()));
}

TEST_CASE_FIXTURE(FddTmpDir, "msa: write refuses geometry which doesn't match image size")
{
    std::vector<uint8_t> img = makeImageData(80, 2, 9, false);
    CHECK_FALSE(MSA_WriteDisk(path("a.msa").c_str(), img.data(), (int) img.size(), 80, 2, 10));    // size mismatch
    CHECK_FALSE(MSA_WriteDisk(path("a.msa").c_str(), img.data(), (int) img.size(), 80, 0, 9));     // 0 sides
    CHECK_FALSE(MSA_WriteDisk(path("a.msa").c_str(), img.data(), (int) img.size(), 80, 2, 9, 80)); // start track beyond end
}

TEST_CASE_FIXTURE(FddTmpDir, "msa: save keeps loaded geometry, even when boot sector is zeroed")
{
    // geometry used to come from the boot sector - zeroed boot sector + non-standard size = divide by zero
    struct { int tracks, sides, spt; } cases[] = { {80, 2, 9}, {85, 2, 10}, {40, 1, 9}, {84, 2, 12} };

    for(auto &c : cases) {
        CAPTURE(c.tracks);
        CAPTURE(c.sides);
        CAPTURE(c.spt);

        std::vector<uint8_t> img = makeImageData(c.tracks, c.sides, c.spt, c.spt == 12);   // 84/2/12 random: > 1 MB incompressible
        img[19] = 1440 & 0xff; img[20] = 1440 >> 8; img[24] = 9; img[25] = 0; img[26] = 2; img[27] = 0;  // boot sector claims 80/2/9
        REQUIRE(MSA_WriteDisk(path("g.msa").c_str(), img.data(), (int) img.size(), c.tracks, c.sides, c.spt));

        uint8_t zero[512] = {0}, mark[512];
        memset(mark, 0xA5, 512);

        FloppyImage *a = new FloppyImageMsa();
        REQUIRE(a->open(path("g.msa").c_str()));
        REQUIRE(a->writeSector(0, 0, 1, zero));                                 // boot sector zeroed, like after format of track 0
        REQUIRE(a->writeSector(c.tracks - 1, c.sides - 1, c.spt, mark));         // last sector of disk changed
        CHECK(a->save());
        delete a;

        FloppyImageMsa b;
        REQUIRE(b.open(path("g.msa").c_str()));
        int t, s, p;
        b.getParams(t, s, p);
        CHECK(t == c.tracks);
        CHECK(s == c.sides);
        CHECK(p == c.spt);
        CHECK(sectorIs(&b, c.tracks - 1, c.sides - 1, c.spt, mark));
        CHECK(sectorIs(&b, 0, 0, 1, zero));

        int mt = c.tracks / 2, ms = c.sides - 1, mp = (c.spt + 1) / 2;          // untouched sector in the middle
        CHECK(sectorIs(&b, mt, ms, mp, img.data() + sectorIndex(mt, ms, mp, c.sides, c.spt) * 512));
    }
}

TEST_CASE_FIXTURE(FddTmpDir, "msa: starting track is honored on load and save")
{
    const int tracks = 10, sides = 2, spt = 9, bytesPerTrack = sides * spt * 512;
    std::vector<uint8_t> img(tracks * bytesPerTrack, 0);
    for(int t=2; t<tracks; t++) {
        memset(img.data() + t * bytesPerTrack, 0x10 + t, bytesPerTrack);      // every track has its own value
    }
    REQUIRE(MSA_WriteDisk(path("s.msa").c_str(), img.data(), (int) img.size(), tracks, sides, spt, 2));   // file holds tracks 2..9
    REQUIRE(msaStartTrack(path("s.msa")) == 2);

    uint8_t t5[512], zero[512] = {0}, mark[512];
    memset(t5, 0x15, 512);
    memset(mark, 0x77, 512);

    FloppyImage *a = new FloppyImageMsa();
    REQUIRE(a->open(path("s.msa").c_str()));
    int t, s, p;
    a->getParams(t, s, p);
    CHECK(t == 10);                         // tracks 0..9, not 8 tracks starting at 0
    CHECK(sectorIs(a, 5, 1, 3, t5));        // track 5 is at track 5
    CHECK(sectorIs(a, 0, 0, 1, zero));      // tracks not in file are empty

    REQUIRE(a->writeSector(9, 1, 9, mark));
    REQUIRE(a->save());
    CHECK(msaStartTrack(path("s.msa")) == 2);   // nothing written to leading tracks - file still starts at track 2

    REQUIRE(a->writeSector(0, 0, 1, mark));
    REQUIRE(a->save());
    delete a;
    CHECK(msaStartTrack(path("s.msa")) == 0);   // leading track has data now - saved from track 0

    FloppyImageMsa b;
    REQUIRE(b.open(path("s.msa").c_str()));
    CHECK(sectorIs(&b, 0, 0, 1, mark));
    CHECK(sectorIs(&b, 9, 1, 9, mark));
    CHECK(sectorIs(&b, 5, 1, 3, t5));
}

TEST_CASE_FIXTURE(FddTmpDir, "msa: sectors per track are limited")
{
    int spts[]   = {11, 15, MAX_SECTORS_PER_TRACK_IN_STREAM, MAX_SECTORS_PER_TRACK_IN_STREAM + 1, 36};
    bool loads[] = {true, true, true, false, false};

    for(int i=0; i<5; i++) {
        CAPTURE(spts[i]);
        std::vector<uint8_t> img(80 * 2 * spts[i] * 512, 0);
        REQUIRE(MSA_WriteDisk(path("spt.msa").c_str(), img.data(), (int) img.size(), 80, 2, spts[i]));

        FloppyImageMsa msa;
        CHECK(msa.open(path("spt.msa").c_str()) == loads[i]);
    }
}

//----------------------------------------------------------------------
// MFM cached image (encoder + decoder)

TEST_CASE("mfm cache: without image nothing is encoded and nothing crashes")
{
    MfmCachedImage *enc = new MfmCachedImage();

    int track, side;
    CHECK_FALSE(enc->findNotReadyTrackAndEncodeIt(NULL, track, side));     // used to dereference NULL image
    CHECK_FALSE(enc->somethingToBeEncoded());

    enc->askToReencodeTrack(0, 0);                                          // ignored without image
    CHECK_FALSE(enc->somethingToBeEncoded());

    std::vector<uint8_t> buf(MFM_STREAM_SIZE);
    int bytes = -1;
    CHECK_FALSE(enc->copyEncodedTrackIfReady(0, 0, buf.data(), bytes));
    CHECK(bytes == -1);                                                     // untouched when not copied

    int index;
    MfmCachedImage::trackAndSideToIndex(-1, -1, index);
    CHECK(index == -1);
    MfmCachedImage::trackAndSideToIndex(0, 2, index);
    CHECK(index == -1);
    MfmCachedImage::trackAndSideToIndex(85, 0, index);
    CHECK(index == -1);
    MfmCachedImage::trackAndSideToIndex(84, 1, index);
    CHECK(index == 169);

    delete enc;
}

TEST_CASE_FIXTURE(FddTmpDir, "mfm cache: clearing the cache forgets the previous image")
{
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    MfmCachedImage *enc = new MfmCachedImage();
    enc->storeImageParams(&img, SLOT_COUNT - 1);
    CHECK(enc->somethingToBeEncoded());

    enc->clearWholeCachedImage();           // what happens when a new image is about to be loaded
    int t, s, p;
    enc->getParams(t, s, p);
    CHECK(t == 0);                          // old geometry used to stay -> requests for tracks of a failed new image
    CHECK(s == 0);
    CHECK(p == 0);
    CHECK_FALSE(enc->somethingToBeEncoded());

    delete enc;
}

TEST_CASE_FIXTURE(FddTmpDir, "mfm cache: encoded track can be copied, re-encode request makes it not ready")
{
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    MfmCachedImage *enc = new MfmCachedImage();
    enc->storeImageParams(&img, SLOT_COUNT - 1);
    encodeWholeImage(enc, &img);
    CHECK_FALSE(enc->somethingToBeEncoded());

    std::vector<uint8_t> buf(MFM_STREAM_SIZE);
    int bytes = 0;
    REQUIRE(enc->copyEncodedTrackIfReady(79, 1, buf.data(), bytes));
    CHECK(buf[0] == 79);                    // stream starts with track and side
    CHECK(buf[1] == 1);
    CHECK(bytes > 0);
    CHECK(bytes <= MFM_STREAM_SIZE);

    pthread_mutex_lock(&floppyEncoderMutex);
    enc->askToReencodeTrack(79, 1);
    pthread_mutex_unlock(&floppyEncoderMutex);
    CHECK_FALSE(enc->copyEncodedTrackIfReady(79, 1, buf.data(), bytes));
    CHECK(enc->somethingToBeEncoded());

    encodeWholeImage(enc, &img);
    CHECK(enc->copyEncodedTrackIfReady(79, 1, buf.data(), bytes));

    delete enc;
}

TEST_CASE_FIXTURE(FddTmpDir, "mfm cache: track size to send covers the track + side prefix and the whole stream up to the end mark")
{
    // the device gets exactly 'bytes' bytes - the stream must end with the end mark within them,
    // otherwise the device streams on into whatever is after the received data
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    MfmCachedImage *enc = new MfmCachedImage();
    enc->storeImageParams(&img, SLOT_COUNT - 1);
    encodeWholeImage(enc, &img);

    for(int side=0; side<2; side++) {
        for(int track : { 0, 40, 79 }) {
            CAPTURE(track);
            CAPTURE(side);

            std::vector<uint8_t> buf(MFM_STREAM_SIZE, 0xaa);
            int bytes = 0;
            REQUIRE(enc->copyEncodedTrackIfReady(track, side, buf.data(), bytes));

            REQUIRE(bytes >= 8);
            REQUIRE(bytes <= MFM_STREAM_SIZE);
            CHECK(buf[0] == track);
            CHECK(buf[1] == side);
            CHECK(buf[2 + 2] == MFM_STREAM_VERSION);
            CHECK(((buf[2] << 8) | buf[2 + 1]) == bytes - 2);       // end offset is from the start of the stream, without the prefix
            CHECK(buf[bytes - 2] == MFM_ESC);
            CHECK(buf[bytes - 1] == MFM_OP_END);
        }
    }

    delete enc;
}

// Time the track takes on the floppy: every data byte is 32 us. Escapes and marks take no time.
static int trackDataBytes(const uint8_t *stream)
{
    int end = (stream[0] << 8) | stream[1];
    int pos = MFM_STREAM_HEADER_SIZE;
    int count = 0;

    while(pos < end) {
        if(stream[pos] != MFM_ESC) {
            pos++;
            count++;
            continue;
        }

        uint8_t op = stream[pos + 1];
        pos += 2;

        if(op == MFM_OP_SECTOR) {
            pos += 3;
        } else if(op == MFM_OP_RAW) {
            int size = (stream[pos] << 8) | stream[pos + 1];
            pos += 2 + size;
            count += size;
        } else if(op != MFM_OP_END) {
            count++;
        }
    }

    return count;
}

TEST_CASE_FIXTURE(FddTmpDir, "mfm cache: track fits into one rotation up to 11 sectors, all the sectors are there")
{
    for(int spt : { 9, 10, 11 }) {
        CAPTURE(spt);
        writeFile(path("a.st"), makeImageData(80, 2, spt, false));
        FloppyImageSt img;
        REQUIRE(img.open(path("a.st").c_str()));

        MfmCachedImage *enc = new MfmCachedImage();
        enc->storeImageParams(&img, SLOT_COUNT - 1);
        encodeWholeImage(enc, &img);

        std::vector<uint8_t> buf(MFM_STREAM_SIZE);
        int bytes = 0;
        REQUIRE(enc->copyEncodedTrackIfReady(3, 1, buf.data(), bytes));

        CHECK(trackDataBytes(buf.data() + 2) <= 6250);      // v1 had 6814 B for 11 sectors, the last sector didn't make it

        std::vector<uint8_t> mfm;
        for(int sector=1; sector<=spt; sector++) {
            CHECK(tryGetSectorDataPartMfm(enc, 3, 1, sector, mfm));
        }
        CHECK_FALSE(tryGetSectorDataPartMfm(enc, 3, 1, spt + 1, mfm));

        delete enc;
    }
}

TEST_CASE("track builder: F5 data bytes are escaped, raw runs aren't")
{
    uint8_t bfr[64];
    MfmTrackBuilder b;
    b.begin(bfr, sizeof(bfr));

    const uint8_t raw[] = { 0xF5, 0x12 };
    b.data(0x4e);
    b.data(0xF5);
    b.syncA1();
    b.sectorMark(1, 80, 9);
    b.rawRun(raw, 2);
    b.cellTime(32000);
    int size = b.finish();

    const uint8_t expected[] = { 0, 26, MFM_STREAM_VERSION, 0,
        0x4e, MFM_ESC, MFM_OP_LITERAL, MFM_ESC, MFM_OP_SYNC_A1,
        MFM_ESC, MFM_OP_SECTOR, 1, 80, 9,
        MFM_ESC, MFM_OP_RAW, 0, 2, 0xF5, 0x12,
        MFM_ESC, MFM_OP_CELL_TIME, 32000 >> 8, 32000 & 0xff,
        MFM_ESC, MFM_OP_END };

    REQUIRE(size == (int) sizeof(expected));
    CHECK(memcmp(bfr, expected, size) == 0);
    CHECK_FALSE(b.overflowed());
}

TEST_CASE("track builder: what doesn't fit is left out whole, the stream stays valid and in the buffer")
{
    std::vector<uint8_t> raw(600, 0x11);

    for(int bufferSize : { 6, 7, 20, 603, 609, 610, 611, 700 }) {
        CAPTURE(bufferSize);

        std::vector<uint8_t> bfr(bufferSize + 1, 0xaa);     // the last one is a guard
        MfmTrackBuilder b;
        b.begin(bfr.data(), bufferSize);

        b.rawRun(raw.data(), raw.size());
        b.data(0xF5);                                       // 2 B when escaped
        b.data(0x22);
        int size = b.finish();

        CHECK(size <= bufferSize);
        CHECK(bfr[bufferSize] == 0xaa);
        CHECK(((bfr[0] << 8) | bfr[1]) == size);
        CHECK(bfr[size - 2] == MFM_ESC);
        CHECK(bfr[size - 1] == MFM_OP_END);

        bool all = (bufferSize >= 4 + 4 + 600 + 2 + 1 + 2);
        CHECK(b.overflowed() == !all);

        if(bufferSize < 4 + 4 + 600 + 2) {                  // nothing after the item that didn't fit
            CHECK(size == 4 + 2);
        }
    }
}

TEST_CASE_FIXTURE(FddTmpDir, "mfm: encoded sector decodes back to the same data")
{
    std::vector<uint8_t> data = makeImageData(80, 2, 9, true);
    writeFile(path("a.st"), data);
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    MfmCachedImage *enc = new MfmCachedImage();
    enc->storeImageParams(&img, SLOT_COUNT - 1);
    encodeWholeImage(enc, &img);

    struct { int track, side, sector; } sectors[] = { {0, 0, 1}, {40, 1, 5}, {79, 1, 9} };
    for(auto &s : sectors) {
        CAPTURE(s.track);
        CAPTURE(s.sector);

        std::vector<uint8_t> mfm = getSectorDataPartMfm(enc, s.track, s.side, s.sector);
        REQUIRE(mfm.size() <= WRITTENMFMSECTOR_SIZE);

        uint8_t out[512];
        memset(out, 0, 512);
        CHECK(decodeSector(enc, mfm, out));
        CHECK_FALSE(enc->lastBufferWasFormatTrack());
        CHECK(memcmp(out, data.data() + sectorIndex(s.track, s.side, s.sector, 2, 9) * 512, 512) == 0);
    }

    // damaged data - CRC must not match
    std::vector<uint8_t> mfm = getSectorDataPartMfm(enc, 40, 1, 5);
    mfm[300] ^= 0xFF;           // somewhere in the middle of the data
    uint8_t out[512];
    CHECK_FALSE(decodeSector(enc, mfm, out));

    delete enc;
}

//----------------------------------------------------------------------
// floppy encoder thread - written sectors, image switch, eject, shutdown

static std::vector<uint8_t> mfmOfSectorFilledWith(const std::string &dir, uint8_t value, int track, int side, int sector)
{
    // encode an image which has the wanted content in that sector, take the MFM of its data part = what the device sends on write
    std::vector<uint8_t> data(80 * 2 * 9 * 512, 0);
    memset(data.data() + sectorIndex(track, side, sector, 2, 9) * 512, value, 512);
    std::string path = dir + "/src.st";
    writeFile(path, data);

    FloppyImageSt img;
    REQUIRE(img.open(path.c_str()));
    MfmCachedImage *enc = new MfmCachedImage();
    enc->storeImageParams(&img, SLOT_COUNT - 1);
    encodeWholeImage(enc, &img);
    std::vector<uint8_t> mfm = getSectorDataPartMfm(enc, track, side, sector);
    delete enc;
    return mfm;
}

template<typename F> static bool waitFor(F condition, int timeoutMs = 10000, int pollMs = 10)
{
    for(int i=0; i<timeoutMs / pollMs; i++) {
        if(condition()) {
            return true;
        }
        Utils::sleepMs(pollMs);
    }
    return condition();
}

static uint32_t lockedImageGen(int slot)
{
    pthread_mutex_lock(&floppyEncoderMutex);
    uint32_t gen = slots[slot].imageGen;
    pthread_mutex_unlock(&floppyEncoderMutex);
    return gen;
}

static void deviceReceivedCurrentImage(int slot)        // what CoreFdd does after sending the whole image to the device
{
    pthread_mutex_lock(&floppyEncoderMutex);
    slots[slot].deviceGen = slots[slot].imageGen;
    pthread_mutex_unlock(&floppyEncoderMutex);
}

static bool fileSectorIs(const std::string &path, int track, int side, int sector, uint8_t value)
{
    std::vector<uint8_t> d = readFile(path);
    size_t off = sectorIndex(track, side, sector, 2, 9) * 512;
    if(d.size() < off + 512) {
        return false;
    }
    for(int i=0; i<512; i++) {
        if(d[off + i] != value) {
            return false;
        }
    }
    return true;
}

TEST_CASE_FIXTURE(FddTmpDir, "floppy encoder: written sectors go to the image the device has, also during image switch")
{
    // The encoder thread can run only once per process (stop flag is never reset), so this is the only test running it.
    const int slot = 0;
    std::string pathA = path("A.st"), pathB = path("B.st");
    writeFile(pathA, std::vector<uint8_t>(80 * 2 * 9 * 512, 0));
    writeFile(pathB, std::vector<uint8_t>(80 * 2 * 9 * 512, 0));

    std::vector<uint8_t> mfm5A = mfmOfSectorFilledWith(dir, 0x5A, 3, 1, 4);
    std::vector<uint8_t> mfmA7 = mfmOfSectorFilledWith(dir, 0xA7, 5, 0, 2);
    std::vector<uint8_t> mfm3C = mfmOfSectorFilledWith(dir, 0x3C, 6, 1, 7);

    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, floppyEncodeThreadCode, NULL) == 0);

    // load image A, wait until fully encoded, device receives it
    slotSendToDevice[slot] = 0;
    floppyEncoder_addEncodeWholeImageRequest(slot, pathA.c_str());
    REQUIRE(waitFor([&]{ return slotSendToDevice[slot] != 0; }));
    slotSendToDevice[slot] = 0;
    deviceReceivedCurrentImage(slot);

    // ST writes a sector to A - written to image A, its track re-encoded with the new data
    // (decode with own decoder object - the slot's one is used by the encoder thread at the same time)
    // Until then the track must not be ready - a track sent to the device meanwhile would have the old data.
    MfmCachedImage *decoder = new MfmCachedImage();
    floppyEncoder_decodeMfmWrittenSector(slot, 3, 1, 4, mfm5A.data(), mfm5A.size());
    bool readyWithOldData = false;
    bool reencoded = waitFor([&]{
        std::vector<uint8_t> mfm;
        uint8_t out[512];
        if(!tryGetSectorDataPartMfm(&slots[slot].encImage, 3, 1, 4, mfm) || !decodeSector(decoder, mfm, out)) {
            return false;                       // not ready yet
        }
        if(out[0] == 0x00 && out[511] == 0x00) {
            readyWithOldData = true;            // ready, but the write isn't in it
        }
        return out[0] == 0x5A && out[511] == 0x5A;
    }, 10000, 1);
    delete decoder;
    REQUIRE(reencoded);
    CHECK_FALSE(readyWithOldData);

    // switch to image B - device still has A until B is sent to it
    uint32_t genA = lockedImageGen(slot);
    floppyEncoder_addEncodeWholeImageRequest(slot, pathB.c_str());
    REQUIRE(waitFor([&]{ return lockedImageGen(slot) != genA && slotSendToDevice[slot] != 0; }));
    slotSendToDevice[slot] = 0;
    CHECK(fileSectorIs(pathA, 3, 1, 4, 0x5A) == false);    // A still kept in memory for the device, not saved yet (no 5 s idle passed)

    // ST writes while device still has A -> must go to A, not to the new image B
    floppyEncoder_decodeMfmWrittenSector(slot, 5, 0, 2, mfmA7.data(), mfmA7.size());
    Utils::sleepMs(200);

    // device receives B -> old image A is released = saved with both writes
    deviceReceivedCurrentImage(slot);
    REQUIRE(waitFor([&]{ return fileSectorIs(pathA, 5, 0, 2, 0xA7); }));
    CHECK(fileSectorIs(pathA, 3, 1, 4, 0x5A));

    // ST writes to B now
    floppyEncoder_decodeMfmWrittenSector(slot, 6, 1, 7, mfm3C.data(), mfm3C.size());
    Utils::sleepMs(200);

    // eject - B is saved, device gets empty slot
    floppyEncoder_addEncodeWholeImageRequest(slot, "");
    REQUIRE(waitFor([&]{ return slotSendToDevice[slot] != 0; }));
    slotSendToDevice[slot] = 0;
    int t, s, p;
    slots[slot].encImage.getParams(t, s, p);
    CHECK(t == 0);                                          // empty slot is sent as 0 tracks

    // image C is loaded, then the ST writes to it - the same request for the unchanged file does nothing
    std::string pathC = path("C.st"), pathNew = path("new.st");
    writeFile(pathC, std::vector<uint8_t>(80 * 2 * 9 * 512, 0x00));
    floppyEncoder_addEncodeWholeImageRequest(slot, pathC.c_str());
    REQUIRE(waitFor([&]{ return slotSendToDevice[slot] != 0; }));
    slotSendToDevice[slot] = 0;
    deviceReceivedCurrentImage(slot);

    uint32_t genC = lockedImageGen(slot);
    floppyEncoder_addEncodeWholeImageRequest(slot, pathC.c_str());
    Utils::sleepMs(200);
    CHECK(lockedImageGen(slot) == genC);

    floppyEncoder_decodeMfmWrittenSector(slot, 3, 1, 4, mfm5A.data(), mfm5A.size());
    Utils::sleepMs(200);

    // the file gets replaced (web UI: create empty image over it), inserted again: loaded and sent to the device, and
    // the write to the old content doesn't get into the new file
    writeFile(pathNew, std::vector<uint8_t>(80 * 2 * 9 * 512, 0x77));
    REQUIRE(rename(pathNew.c_str(), pathC.c_str()) == 0);
    floppyEncoder_addEncodeWholeImageRequest(slot, pathC.c_str());
    REQUIRE(waitFor([&]{ return lockedImageGen(slot) != genC && slotSendToDevice[slot] != 0; }));
    slotSendToDevice[slot] = 0;
    deviceReceivedCurrentImage(slot);

    MfmCachedImage *decoderC = new MfmCachedImage();
    REQUIRE(waitFor([&]{
        std::vector<uint8_t> mfm;
        uint8_t out[512];
        return tryGetSectorDataPartMfm(&slots[slot].encImage, 3, 1, 4, mfm) && decodeSector(decoderC, mfm, out) && out[0] == 0x77;
    }));
    delete decoderC;

    Utils::sleepMs(200);                                    // the old image got released and it would save now
    CHECK(fileSectorIs(pathC, 3, 1, 4, 0x77));

    // shutdown - everything saved
    floppyEncoder_stop();
    pthread_join(thread, NULL);

    CHECK(fileSectorIs(pathB, 6, 1, 7, 0x3C));
    CHECK(fileSectorIs(pathB, 5, 0, 2, 0x00));             // write meant for A didn't end up in B
    CHECK(fileSectorIs(pathA, 6, 1, 7, 0x00));             // write meant for B didn't end up in A
}

TEST_CASE_FIXTURE(FddTmpDir, "floppy encoder: track with a received written sector is not ready until the write is in it")
{
    // Without the encoder thread: only the immediate effect of receiving a written sector. The device may ask for the
    // whole image right after sending writes (it resends the ones kept while disconnected first), and that must not
    // get the old content of the written track.
    const int slot = 1;
    writeFile(path("a.st"), makeImageData(80, 2, 9, false));
    FloppyImageSt img;
    REQUIRE(img.open(path("a.st").c_str()));

    MfmCachedImage *enc = &slots[slot].encImage;
    enc->storeImageParams(&img, slot);
    encodeWholeImage(enc, &img);

    std::vector<uint8_t> buf(MFM_STREAM_SIZE);
    int bytes = 0;
    REQUIRE(enc->copyEncodedTrackIfReady(3, 1, buf.data(), bytes));

    std::vector<uint8_t> mfm = mfmOfSectorFilledWith(dir, 0x5A, 3, 1, 4);
    floppyEncoder_decodeMfmWrittenSector(slot, 3, 1, 4, mfm.data(), mfm.size());

    CHECK_FALSE(enc->copyEncodedTrackIfReady(3, 1, buf.data(), bytes));     // written track - wait for re-encode
    CHECK(enc->copyEncodedTrackIfReady(3, 0, buf.data(), bytes));           // other side, other tracks - still ready
    CHECK(enc->copyEncodedTrackIfReady(4, 1, buf.data(), bytes));

    enc->clearWholeCachedImage();           // leave the slot as other tests expect it
}
