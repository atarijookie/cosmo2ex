// Tests of GEMDOS file attributes on a non-FAT drive (FAT attributes need a vfat mount, not available here):
// FA_DIR, FA_READONLY from write permission, FA_HIDDEN from name starting with '.'

#include <sys/stat.h>
#include <map>

#include "test_helpers.h"

static mode_t modeOf(const std::string& path)
{
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

// name -> attributes of everything found for the search path
static std::map<std::string, uint8_t> findAll(const std::string& path)
{
    std::map<std::string, uint8_t> found;
    SearchParams sp;
    sp.path = path;
    sp.attribs = 0xff;
    sp.addUpDir = true;
    sp.internal = NULL;

    DiskItem di;
    while(ldp_findFirstAndNext(sp, di)) {
        found[di.name] = di.attribs;
    }
    return found;
}

TEST_CASE_FIXTURE(ScratchDir, "ldp_getAttributes - dir, read only from write permission, hidden from leading dot")
{
    sh("mkdir -p " + p("d/sub") + " " + p("d/.hiddendir"));
    sh("touch " + p("d/normal") + " " + p("d/readonly") + " " + p("d/.hidden"));
    sh("chmod 444 " + p("d/readonly"));

    uint8_t a;
    REQUIRE(ldp_getAttributes(p("d/normal"), a));       CHECK(a == 0);
    REQUIRE(ldp_getAttributes(p("d/readonly"), a));     CHECK(a == FA_READONLY);
    REQUIRE(ldp_getAttributes(p("d/.hidden"), a));      CHECK(a == FA_HIDDEN);
    REQUIRE(ldp_getAttributes(p("d/sub"), a));          CHECK(a == FA_DIR);
    REQUIRE(ldp_getAttributes(p("d/sub/"), a));         CHECK(a == FA_DIR);         // trailing separator
    REQUIRE(ldp_getAttributes(p("d/.hiddendir"), a));   CHECK(a == (FA_DIR | FA_HIDDEN));
    REQUIRE(ldp_getAttributes(p("d/sub/.."), a));       CHECK(a == FA_DIR);         // '..' is not hidden

    CHECK_FALSE(ldp_getAttributes(p("d/notthere"), a));
}

TEST_CASE_FIXTURE(ScratchDir, "ldp_setAttributes - read only is write permission, the rest is ignored without error")
{
    sh("mkdir -p " + p("d"));
    sh("touch " + p("d/f"));
    sh("chmod 664 " + p("d/f"));

    REQUIRE(ldp_setAttributes(p("d/f"), FA_READONLY));
    CHECK(modeOf(p("d/f")) == 0444);                    // all write permissions removed

    uint8_t a;
    REQUIRE(ldp_getAttributes(p("d/f"), a));
    CHECK(a == FA_READONLY);

    REQUIRE(ldp_setAttributes(p("d/f"), 0));
    CHECK(modeOf(p("d/f")) == 0644);                    // owner can write again

    REQUIRE(ldp_setAttributes(p("d/f"), FA_HIDDEN | FA_SYSTEM | FA_ARCHIVE));    // can't be stored here - ignored, no error
    CHECK(modeOf(p("d/f")) == 0644);
    REQUIRE(ldp_getAttributes(p("d/f"), a));
    CHECK(a == 0);

    CHECK_FALSE(ldp_setAttributes(p("d/notthere"), FA_READONLY));
}

TEST_CASE_FIXTURE(ScratchDir, "ldp_findFirstAndNext reports the same attributes as ldp_getAttributes")
{
    sh("mkdir -p " + p("d/SUB") + " " + p("d/.HDIR"));
    sh("touch " + p("d/NORMAL") + " " + p("d/RDONLY") + " " + p("d/.HFILE"));
    sh("chmod 444 " + p("d/RDONLY"));

    std::map<std::string, uint8_t> found = findAll(p("d/*.*"));

    CHECK(found["NORMAL"]   == 0);
    CHECK(found["RDONLY"]   == FA_READONLY);
    CHECK(found["SUB"]      == FA_DIR);
    CHECK(found["."]        == FA_DIR);                 // up-dirs are not hidden (used to be)
    CHECK(found[".."]       == FA_DIR);
    CHECK((found.size() == 7));                         // + the 2 hidden items, under their short names

    for(auto &f : found) {
        if(f.first[0] == '.' && f.first != "." && f.first != "..") {
            CAPTURE(f.first);
            CHECK((f.second & FA_HIDDEN) != 0);
        }
    }
}

TEST_CASE_FIXTURE(ScratchDir, "ldp_findFirstAndNext - attributes of entries not matching the pattern don't leak into the next match")
{
    sh("mkdir -p " + p("d"));
    sh("touch " + p("d/GAME.PRG"));
    for(int i=0; i<20; i++) {                           // dir order is not known - with this many, some come before GAME.PRG
        sh("mkdir " + p("d/.hidden" + std::to_string(i)));
    }

    std::map<std::string, uint8_t> found = findAll(p("d/*.PRG"));

    REQUIRE(found.count("GAME.PRG") == 1);
    CHECK(found["GAME.PRG"] == 0);                      // used to get FA_DIR | FA_HIDDEN of the skipped dirs
}
