/*
    Tests of the image list for browsing the floppy images on the device - fdd/imagelist.cpp
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <algorithm>

#include "doctest.h"

#include "misc/utils.h"
#include "fdd/imagelist.h"

namespace {

struct ListTmpDir {
    std::string dir;

    ListTmpDir() {
        char tmpl[] = "/tmp/ce_test_imagelist_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
    }

    ~ListTmpDir() {
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }

    std::string path(const std::string& name) { return dir + "/" + name; }

    void touch(const std::string& name) {
        FILE* f = fopen(path(name).c_str(), "w");
        REQUIRE(f != NULL);
        fclose(f);
    }

    // folder mtime moved to the past, so the list doesn't see it as racy (changed right before the scan)
    void ageFolder(int secondsAgo = 60) {
        struct timeval tv[2];
        gettimeofday(&tv[0], NULL);
        tv[0].tv_sec -= secondsAgo;
        tv[1] = tv[0];
        REQUIRE(utimes(dir.c_str(), tv) == 0);
    }
};

std::vector<std::string> allNames(const ImageList& list)
{
    std::vector<std::string> v;
    for(uint32_t i = 0; i < list.count(); i++) {
        v.push_back(list.getName(i));
    }
    return v;
}

struct Page {
    uint32_t gen, total, current, start;
    std::vector<std::string> names;
};

Page parsePage(const uint8_t* bfr, uint32_t len)
{
    Page p;
    REQUIRE(len >= IMAGE_LIST_PAGE_HEADER);
    p.gen     = Utils::getDword((uint8_t*) bfr);
    p.total   = Utils::getDword((uint8_t*) bfr + 4);
    p.current = Utils::getDword((uint8_t*) bfr + 8);
    p.start   = Utils::getDword((uint8_t*) bfr + 12);
    uint16_t count = Utils::getWord((uint8_t*) bfr + 16);

    uint32_t pos = IMAGE_LIST_PAGE_HEADER;
    for(int i = 0; i < count; i++) {
        REQUIRE(pos < len);
        uint8_t l = bfr[pos++];
        REQUIRE(pos + l <= len);
        p.names.push_back(std::string((const char*) bfr + pos, l));
        pos += l;
    }
    CHECK(pos == len);
    return p;
}

Page page(const ImageList& list, uint32_t start, uint32_t count, uint32_t current)
{
    static uint8_t bfr[IMAGE_LIST_PAGE_MAX_SIZE];
    uint32_t len = list.storePage(bfr, start, count, current);
    CHECK(len <= IMAGE_LIST_PAGE_MAX_SIZE);
    return parsePage(bfr, len);
}

}   // namespace

TEST_CASE("image list: only .st / .msa names, any case")
{
    CHECK(ImageList::isImageName("game.st"));
    CHECK(ImageList::isImageName("GAME.ST"));
    CHECK(ImageList::isImageName("disk 1.Msa"));
    CHECK(ImageList::isImageName("a.b.st"));
    CHECK_FALSE(ImageList::isImageName("game.stx"));
    CHECK_FALSE(ImageList::isImageName("game.zip"));
    CHECK_FALSE(ImageList::isImageName("readme"));
    CHECK_FALSE(ImageList::isImageName(".st"));
    CHECK_FALSE(ImageList::isImageName("st"));
}

TEST_CASE("image list: sorted case insensitive, names differing only in case by bytes")
{
    std::vector<std::string> v = { "zork.st", "Arkanoid.st", "arkanoid.st", "B.st", "a.st", "ab.st", "A.st", "_x.st", "1.st" };
    std::sort(v.begin(), v.end(), ImageList::nameLess);

    // by the lower case bytes: '1' (0x31) < '_' (0x5f) < letters (0x61..); the same apart from case - upper case first
    std::vector<std::string> expected = { "1.st", "_x.st", "A.st", "a.st", "ab.st", "Arkanoid.st", "arkanoid.st", "B.st", "zork.st" };
    CHECK(v == expected);
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: folder scan takes image files only, sorted")
{
    touch("Zork.st");
    touch("arkanoid.MSA");
    touch("readme.txt");
    touch("Barbarian.st");
    REQUIRE(mkdir(path("dir.st").c_str(), 0755) == 0);                   // a dir, even with an image name
    REQUIRE(symlink(path("Zork.st").c_str(), path("link.st").c_str()) == 0);        // symlink to a file - is an image
    REQUIRE(symlink(path("dir.st").c_str(), path("linkdir.st").c_str()) == 0);      // symlink to a dir - not
    REQUIRE(symlink(path("missing.st").c_str(), path("broken.st").c_str()) == 0);   // broken symlink - not

    ImageList list;
    CHECK(list.setFolder(dir));
    CHECK(list.getFolder() == dir);

    std::vector<std::string> expected = { "arkanoid.MSA", "Barbarian.st", "link.st", "Zork.st" };
    CHECK(allNames(list) == expected);

    CHECK(list.indexOf("Barbarian.st") == 1);
    CHECK(list.indexOf("barbarian.st") == IMAGE_LIST_NONE);    // exact name only
    CHECK(list.indexOf("readme.txt") == IMAGE_LIST_NONE);
    CHECK(list.getName(99) == "");
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: other folder is a change, the same one isn't")
{
    ImageList list;
    uint32_t gen0 = list.getGen();

    CHECK(list.setFolder(dir));
    uint32_t gen1 = list.getGen();
    CHECK(gen1 != gen0);

    CHECK_FALSE(list.setFolder(dir));
    CHECK(list.getGen() == gen1);

    CHECK(list.setFolder(""));                  // nothing to browse
    CHECK(list.count() == 0);
    CHECK(list.getGen() != gen1);
    CHECK_FALSE(list.refresh());
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: added, deleted, renamed image is found by refresh")
{
    touch("b.st");
    ageFolder();

    ImageList list;
    list.setFolder(dir);
    CHECK(list.count() == 1);
    uint32_t gen = list.getGen();

    CHECK_FALSE(list.refresh());                // nothing changed
    CHECK(list.getGen() == gen);

    touch("a.st");                              // added
    CHECK(list.refresh());
    CHECK(list.getGen() != gen);
    CHECK(list.indexOf("a.st") == 0);
    CHECK(list.indexOf("b.st") == 1);

    gen = list.getGen();
    REQUIRE(rename(path("a.st").c_str(), path("c.st").c_str()) == 0);      // renamed
    CHECK(list.refresh());
    CHECK(list.getGen() != gen);
    CHECK(allNames(list) == std::vector<std::string>({ "b.st", "c.st" }));

    gen = list.getGen();
    REQUIRE(unlink(path("b.st").c_str()) == 0);                             // deleted
    CHECK(list.refresh());
    CHECK(allNames(list) == std::vector<std::string>({ "c.st" }));

    gen = list.getGen();
    touch("notes.txt");                         // not an image - folder changed, but the list didn't
    CHECK_FALSE(list.refresh());
    CHECK(list.getGen() == gen);
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: change with the same folder mtime is found while the scan is racy")
{
    touch("a.st");
    ageFolder(0);                               // mtime = now - like a change right before the scan

    ImageList list;
    list.setFolder(dir);
    uint32_t gen = list.getGen();

    struct stat before;
    REQUIRE(stat(dir.c_str(), &before) == 0);
    touch("b.st");
#if defined(__APPLE__)
    struct timespec ts[2] = { before.st_mtimespec, before.st_mtimespec };  // the same mtime as before - like the 2 s tick of FAT
#else
    struct timespec ts[2] = { before.st_mtim, before.st_mtim };    // the same mtime as before - like the 2 s tick of FAT
#endif
    REQUIRE(utimensat(AT_FDCWD, dir.c_str(), ts, 0) == 0);

    CHECK(list.refresh());                      // scanned again, as the last scan was racy
    CHECK(list.getGen() != gen);
    CHECK(list.count() == 2);
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: deleted folder empties the list")
{
    touch("a.st");
    ImageList list;
    list.setFolder(dir);
    CHECK(list.count() == 1);

    system(("rm -rf '" + dir + "'").c_str());
    CHECK(list.refresh());
    CHECK(list.count() == 0);
    CHECK_FALSE(list.refresh());
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: page - explicit start, around current, moved to fit, short list")
{
    char name[16];
    for(int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "g%02d.st", i);
        touch(name);
    }

    ImageList list;
    list.setFolder(dir);

    Page p = page(list, 5, 16, 7);
    CHECK(p.gen == list.getGen());
    CHECK(p.total == 40);
    CHECK(p.current == 7);
    CHECK(p.start == 5);
    REQUIRE(p.names.size() == 16);
    CHECK(p.names[0] == "g05.st");
    CHECK(p.names[15] == "g20.st");

    p = page(list, IMAGE_LIST_NONE, 16, 20);        // around current
    CHECK(p.start == 12);
    CHECK(p.names[8] == "g20.st");

    p = page(list, IMAGE_LIST_NONE, 16, 3);         // around current, near the start
    CHECK(p.start == 0);

    p = page(list, IMAGE_LIST_NONE, 16, IMAGE_LIST_NONE);   // current not in the list
    CHECK(p.start == 0);
    CHECK(p.current == IMAGE_LIST_NONE);

    p = page(list, 35, 16, 0);                      // beyond the end - moved back, still full
    CHECK(p.start == 24);
    CHECK(p.names.size() == 16);
    CHECK(p.names[15] == "g39.st");

    p = page(list, 1000, 16, 0);
    CHECK(p.start == 24);

    p = page(list, 0, 100, 0);                      // max page size
    CHECK(p.names.size() == IMAGE_LIST_PAGE_MAX);

    p = page(list, 0, 16, 99);                      // current beyond the list = none
    CHECK(p.current == IMAGE_LIST_NONE);

    ImageList empty;
    p = page(empty, 5, 16, 0);
    CHECK(p.total == 0);
    CHECK(p.start == 0);
    CHECK(p.names.empty());
}

TEST_CASE_FIXTURE(ListTmpDir, "image list: the longest names fit the page")
{
    std::string longName = std::string(IMAGE_LIST_NAME_MAX - 3, 'x') + ".st";     // 255 bytes - longest possible on most fs
    touch(longName);
    touch("short.st");

    ImageList list;
    list.setFolder(dir);
    CHECK(list.count() == 2);

    Page p = page(list, 0, 16, 0);
    REQUIRE(p.names.size() == 2);
    CHECK(p.names[0] == "short.st");
    CHECK(p.names[1] == longName);
}
