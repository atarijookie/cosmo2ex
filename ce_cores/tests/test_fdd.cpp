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

// Get the MFM data part of one encoded sector, as the device would send it on sector write:
// everything after the CMD_DATA_PART_OF_SECTOR mark till the end of the fixed size sector.
// (CMD_DATA_PART_OF_SECTOR 0xC0 contains '00' bit pairs, which never appear in MFM time bytes, so it can be searched for.)
static bool tryGetSectorDataPartMfm(MfmCachedImage *enc, int track, int side, int sector, std::vector<uint8_t> &mfm)
{
    std::vector<uint8_t> trackBuf(MFM_STREAM_SIZE);
    int bytes = 0;
    if(!enc->copyEncodedTrackIfReady(track, side, trackBuf.data(), bytes)) {    // copy is taken under mutex - safe while encoder runs
        return false;
    }

    int tableOffset = 2 + 2 * sector;                                       // track + side bytes, then stream table words
    int sectorStart = 2 + ((trackBuf[tableOffset] << 8) | trackBuf[tableOffset + 1]);
    int sectorEnd   = sectorStart + ENCODED_SECTOR_MAX_SIZE;
    if(sectorEnd > MFM_STREAM_SIZE || trackBuf[sectorStart] != CMD_CURRENT_SECTOR || trackBuf[sectorStart + 3] != sector) {
        return false;
    }

    int pos = sectorStart + 4;
    while(pos < sectorEnd && trackBuf[pos] != CMD_DATA_PART_OF_SECTOR) {
        pos++;
    }
    if(pos >= sectorEnd) {
        return false;
    }

    mfm.assign(trackBuf.begin() + pos + 1, trackBuf.begin() + sectorEnd);
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

    std::vector<uint8_t> odd(1000 * 512 + 100);         // matches no tracks * sides * spt combination
    writeFile(path("odd.st"), odd);
    FloppyImageSt bad;
    CHECK_FALSE(bad.open(path("odd.st").c_str()));
    CHECK_FALSE(bad.isLoaded());
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

TEST_CASE_FIXTURE(FddTmpDir, "msa: sectors per track are limited by the stream table")
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

template<typename F> static bool waitFor(F condition, int timeoutMs = 10000)
{
    for(int i=0; i<timeoutMs / 10; i++) {
        if(condition()) {
            return true;
        }
        Utils::sleepMs(10);
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
    MfmCachedImage *decoder = new MfmCachedImage();
    floppyEncoder_decodeMfmWrittenSector(slot, 3, 1, 4, mfm5A.data(), mfm5A.size());
    bool reencoded = waitFor([&]{
        std::vector<uint8_t> mfm;
        uint8_t out[512];
        return tryGetSectorDataPartMfm(&slots[slot].encImage, 3, 1, 4, mfm) &&
               decodeSector(decoder, mfm, out) && out[0] == 0x5A && out[511] == 0x5A;
    });
    delete decoder;
    REQUIRE(reencoded);

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

    // shutdown - everything saved
    floppyEncoder_stop();
    pthread_join(thread, NULL);

    CHECK(fileSectorIs(pathB, 6, 1, 7, 0x3C));
    CHECK(fileSectorIs(pathB, 5, 0, 2, 0x00));             // write meant for A didn't end up in B
    CHECK(fileSectorIs(pathA, 6, 1, 7, 0x00));             // write meant for B didn't end up in A
}
