// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of the translated boot media - CE_DD boot sectors + ce_dd.prg served to the ST on boot

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "hdd/native/translatedbootmedia.h"

// boot sectors and driver in a temp dir, PATH_CE_DD_* pointing to them
struct BootFiles {
    std::string dir, oldL1, oldL2, oldPrg;

    BootFiles() {
        char tmpl[] = "/tmp/ce_test_boot_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;

        oldL1 = PATH_CE_DD_BS_L1;
        oldL2 = PATH_CE_DD_BS_L2;
        oldPrg = PATH_CE_DD_PRG_PATH_AND_FILENAME;
        PATH_CE_DD_BS_L1 = dir + "/ce_dd.bs";
        PATH_CE_DD_BS_L2 = dir + "/ce_dd_l2.bs";
        PATH_CE_DD_PRG_PATH_AND_FILENAME = dir + "/ce_dd.prg";

        std::vector<uint8_t> bs(512, 0);
        bs[100] = 'X';                          // config position marker the boot media looks for
        bs[101] = 'X';
        write(PATH_CE_DD_BS_L1, bs);
        write(PATH_CE_DD_BS_L2, std::vector<uint8_t>(512, 0));
    }

    ~BootFiles() {
        PATH_CE_DD_BS_L1 = oldL1;
        PATH_CE_DD_BS_L2 = oldL2;
        PATH_CE_DD_PRG_PATH_AND_FILENAME = oldPrg;
        system(("rm -rf '" + dir + "'").c_str());
    }

    static void write(const std::string& path, const std::vector<uint8_t>& data) {
        FILE *f = fopen(path.c_str(), "wb");
        REQUIRE(f != NULL);
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }

    // driver of given size, filled with the given byte; mtime moved, so the change is seen also within the same second
    void writeDriver(size_t size, uint8_t fill, time_t mtime) {
        std::vector<uint8_t> prg(size, fill);
        prg[0] = 0x60;                          // PRG header magic, sizes of text / data / bss are 0
        prg[1] = 0x1a;
        memset(&prg[2], 0, 12);
        write(PATH_CE_DD_PRG_PATH_AND_FILENAME, prg);

        struct utimbuf t = {mtime, mtime};
        REQUIRE(utime(PATH_CE_DD_PRG_PATH_AND_FILENAME.c_str(), &t) == 0);
    }
};

static int64_t sectorsOf(TranslatedBootMedia& m)
{
    int64_t bytes, sectors;
    m.getCapacity(bytes, sectors);
    return sectors;
}

TEST_CASE("TranslatedBootMedia reloads the driver on boot sector read when the files changed")
{
    BootFiles f;
    f.writeDriver(1000, 0xaa, 1000000);         // 2 sectors of driver

    TranslatedBootMedia m;
    REQUIRE(m.isInit());
    CHECK(sectorsOf(m) == 4);                   // 2 boot sectors + 2 of driver

    uint8_t bfr[512];
    REQUIRE(m.readSectors(2, 1, bfr));
    CHECK(bfr[100] == 0xaa);

    // new driver built - bigger, other content
    f.writeDriver(1600, 0xbb, 2000000);

    REQUIRE(m.readSectors(2, 1, bfr));          // not a boot (sector 0 not read) - still the old one
    CHECK(bfr[100] == 0xaa);

    REQUIRE(m.readSectors(0, 1, bfr));          // boot sector read = ST boots - new driver is picked up
    CHECK(sectorsOf(m) == 6);
    REQUIRE(m.readSectors(2, 1, bfr));
    CHECK(bfr[100] == 0xbb);

    // driver missing (e.g. being rebuilt) - the loaded one stays
    unlink(PATH_CE_DD_PRG_PATH_AND_FILENAME.c_str());
    REQUIRE(m.readSectors(0, 1, bfr));
    CHECK(sectorsOf(m) == 6);
    REQUIRE(m.readSectors(2, 1, bfr));
    CHECK(bfr[100] == 0xbb);

    // and when it's back, it's loaded
    f.writeDriver(600, 0xcc, 3000000);
    REQUIRE(m.readSectors(0, 1, bfr));
    CHECK(sectorsOf(m) == 4);
    REQUIRE(m.readSectors(2, 1, bfr));
    CHECK(bfr[100] == 0xcc);
}
