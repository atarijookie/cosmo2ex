// Tests of filename case - the same results are expected on case-sensitive filesystems (ext4) and on case-insensitive
// ones (APFS / HFS+ on macOS, FAT, NTFS). Run them on a case-insensitive one by setting CE_TEST_TMPDIR to a dir on it.

#include <fcntl.h>
#include <unistd.h>
#include <map>

#include "test_helpers.h"

// name -> attributes of everything found for the search path
static std::map<std::string, uint8_t> findAll(const std::string& path)
{
    std::map<std::string, uint8_t> found;
    SearchParams sp;
    sp.path = path;
    sp.attribs = 0xff;
    sp.addUpDir = false;
    sp.internal = NULL;

    DiskItem di;
    while(ldp_findFirstAndNext(sp, di)) {
        found[di.name] = di.attribs;
    }
    return found;
}

static std::string longPathOf(const std::string& shortPath)
{
    std::string longPath;
    ldp_shortToLongPath(shortPath, longPath, true);
    return longPath;
}

// true if the filesystem of dir ignores filename case
static bool caseInsensitive(const ScratchDir& s)
{
    sh("mkdir -p " + s.p("probe") + " && touch " + s.p("probe/x"));
    bool res = access(s.p("probe/X").c_str(), F_OK) == 0;
    sh("rm -rf " + s.p("probe"));
    return res;
}

TEST_CASE_FIXTURE(ScratchDir, "file created on the host after the dir was cached - found under its real name, listed once")
{
    sh("mkdir -p " + p("d") + " && touch " + p("d/OTHER.TXT"));
    findAll(p("d/*.*"));                                // dir content is cached now

    sh("touch " + p("d/Game.prg"));                     // e.g. saved by a Mac app while the ST is using the drive

    // on a case-insensitive filesystem 'GAME.PRG' exists too - it used to be taken as the real name, and the
    // next listing then had to call 'Game.prg' 'GAME~1.PRG', as 'GAME.PRG' was taken
    CHECK(longPathOf(p("d/GAME.PRG")) == p("d/Game.prg"));

    std::map<std::string, uint8_t> found = findAll(p("d/*.*"));
    CHECK(found.count("GAME.PRG") == 1);
    CHECK(found.count("GAME~1.PRG") == 0);
    CHECK(found.size() == 2);
}

TEST_CASE_FIXTURE(ScratchDir, "Fsfirst of one file created on the host after the dir was cached - its real name is used")
{
    sh("mkdir -p " + p("d") + " && touch " + p("d/OTHER.TXT"));
    findAll(p("d/*.*"));

    sh("mkdir " + p("d/Games"));

    std::map<std::string, uint8_t> one = findAll(p("d/GAMES"));      // no wildcard - looks for this one item
    CHECK(one.size() == 1);
    CHECK(one.count("GAMES") == 1);

    std::map<std::string, uint8_t> found = findAll(p("d/*.*"));
    CHECK(found.count("GAMES") == 1);
    CHECK(found.count("GAMES~1") == 0);
}

TEST_CASE_FIXTURE(ScratchDir, "lower case names from the ST find the host files, in every part of the path")
{
    sh("mkdir -p " + p("d/Sub") + " && touch " + p("d/Sub/File.txt"));

    // the ST normally sends upper case, but some programs send lower case - DOS names ignore case, so these are
    // the short names SUB and FILE.TXT
    CHECK(longPathOf(p("d/SUB/FILE.TXT")) == p("d/Sub/File.txt"));
    CHECK(longPathOf(p("d/sub/file.txt")) == p("d/Sub/File.txt"));
    CHECK(longPathOf(p("d/Sub/file.TXT")) == p("d/Sub/File.txt"));
}

TEST_CASE_FIXTURE(ScratchDir, "new file - the name stays as the ST sent it")
{
    sh("mkdir -p " + p("d"));
    findAll(p("d/*.*"));

    CHECK(longPathOf(p("d/NEW.TXT")) == p("d/NEW.TXT"));
    sh("touch " + p("d/NEW.TXT"));                      // e.g. Fcreate() with the translated name

    std::map<std::string, uint8_t> found = findAll(p("d/*.*"));
    CHECK(found.count("NEW.TXT") == 1);
    CHECK(found.size() == 1);
}

TEST_CASE_FIXTURE(ScratchDir, "host names in the path are used as they are, also when another name differs only in case")
{
    if(caseInsensitive(*this)) {                        // two such names can't exist there
        return;
    }

    sh("mkdir -p " + p("d/data") + " " + p("d/Data"));
    sh("touch " + p("d/data/a.txt") + " " + p("d/Data/b.txt"));

    // a host path (e.g. the root of the translated drive) names the dir exactly - that one, not the one the short
    // name DATA belongs to
    CHECK(longPathOf(p("d/data/A.TXT")) == p("d/data/a.txt"));
    CHECK(longPathOf(p("d/Data/B.TXT")) == p("d/Data/b.txt"));
}
