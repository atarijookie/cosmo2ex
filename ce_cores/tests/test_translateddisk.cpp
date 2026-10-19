// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of GEMDOS calls of the translated disk - here the file attributes (read only, hidden, dir) on a non-FAT drive

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/settings.h"
#include "hdd/acsidatatrans.h"
#include "hdd/translated/translateddisk.h"
#include "hdd/translated/gemdos.h"
#include "hdd/translated/gemdos_errno.h"

extern std::map<std::string, std::string> dotEnv;

// instead of the device: gives the handler the data the ST would send, keeps the status it returns
class FakeDataTrans : public AcsiDataTrans
{
public:
    std::vector<uint8_t> fromSt;
    uint8_t lastStatus = 0xff;

    bool recvData(uint8_t *data, uint32_t cnt) {
        memset(data, 0, cnt);
        memcpy(data, fromSt.data(), fromSt.size() < cnt ? fromSt.size() : cnt);
        return true;
    }

    void setStatus(uint8_t stat) {
        lastStatus = stat;
        AcsiDataTrans::setStatus(stat);
    }
};

// translated drive D: on a temp dir
struct TranslatedDiskHarness {
    std::string     settingsDir, drive;
    FakeDataTrans   dt;
    THwConfig       hwConfig;
    TranslatedDisk *td;

    TranslatedDiskHarness() {
        // the drive in /tmp, or in $CE_TEST_TMPDIR - to run the tests on another filesystem (e.g. a case-insensitive
        // one, like APFS on macOS)
        const char* tmpDir = getenv("CE_TEST_TMPDIR");
        std::string tmplDrive = std::string((tmpDir && tmpDir[0]) ? tmpDir : "/tmp") + "/ce_test_tdd_XXXXXX";

        char tmplS[] = "/tmp/ce_test_tds_XXXXXX";
        std::vector<char> tmplD(tmplDrive.begin(), tmplDrive.end());
        tmplD.push_back('\0');
        REQUIRE(mkdtemp(tmplS) != NULL);
        REQUIRE(mkdtemp(tmplD.data()) != NULL);
        settingsDir = tmplS;
        drive = tmplD.data();
        dotEnv["SETTINGS_DIR"] = settingsDir;

        uint8_t mac[6] = {0};
        Settings s(mac);
        s.setString("PATH_GEM_D", drive.c_str());

        memset(&hwConfig, 0, sizeof(hwConfig));
        td = new TranslatedDisk(&dt, &hwConfig);
    }

    ~TranslatedDiskHarness() {
        delete td;
        std::string cmd = "chmod -R u+w '" + drive + "'; rm -rf '" + drive + "' '" + settingsDir + "'";
        system(cmd.c_str());
    }

    std::string host(const std::string& name) { return drive + "/" + name; }

    void touch(const std::string& name, mode_t mode = 0644) {
        FILE *f = fopen(host(name).c_str(), "w");
        REQUIRE(f != NULL);
        fclose(f);
        REQUIRE(chmod(host(name).c_str(), mode) == 0);
    }

    mode_t modeOf(const std::string& name) {
        struct stat st;
        REQUIRE(stat(host(name).c_str(), &st) == 0);
        return st.st_mode & 07777;
    }

    // run GEMDOS function, which gets from ST: prefix bytes + Atari path, returns status
    uint8_t call(uint8_t function, std::vector<uint8_t> prefix, const std::string& atariPath, uint8_t cmd5 = 0) {
        dt.fromSt = prefix;
        dt.fromSt.insert(dt.fromSt.end(), atariPath.begin(), atariPath.end());
        dt.fromSt.push_back(0);

        uint8_t cmd[ACSI_CMD_SIZE] = {0, 'C', 'E', HOSTMOD_TRANSLATED_DISK, function, cmd5};
        dt.lastStatus = 0xff;
        td->processCommand(cmd);
        return dt.lastStatus;
    }

    // names in the root dir of the drive, as Fsfirst / Fsnext report them (they get them from libdospath)
    std::vector<std::string> list(void) {
        std::vector<std::string> names;
        SearchParams sp;
        sp.path = drive + "/*.*";
        sp.attribs = 0xff;
        sp.addUpDir = false;
        sp.internal = NULL;

        DiskItem di;
        while(ldp_findFirstAndNext(sp, di)) {
            names.push_back(di.name);
        }
        return names;
    }

    uint8_t fattribGet(const std::string& p)                { return call(GEMDOS_Fattrib, {0, 0}, p); }
    uint8_t fattribSet(const std::string& p, uint8_t a)     { return call(GEMDOS_Fattrib, {1, a}, p); }
};

TEST_CASE("translated disk: Fattrib reports dir, read only (write permission) and hidden (leading dot)")
{
    TranslatedDiskHarness h;
    h.touch("NORMAL");
    h.touch("RDONLY", 0444);
    h.touch(".HIDDEN");
    REQUIRE(mkdir(h.host("SUBDIR").c_str(), 0755) == 0);

    CHECK(h.fattribGet("D:\\NORMAL")    == 0);
    CHECK(h.fattribGet("D:\\RDONLY")    == FA_READONLY);       // used to be 0 - read only was never reported
    CHECK(h.fattribGet("D:\\.HIDDEN")   == FA_HIDDEN);         // used to be 0 - only Fsfirst knew the '.' rule
    CHECK(h.fattribGet("D:\\SUBDIR")    == FA_DIR);
}

TEST_CASE("translated disk: Fattrib set - read only changes write permission, hidden is ignored without error")
{
    TranslatedDiskHarness h;
    h.touch("FILE", 0664);

    CHECK(h.fattribSet("D:\\FILE", FA_READONLY) == 0);          // returns the old attributes
    CHECK(h.modeOf("FILE") == 0444);
    CHECK(h.fattribGet("D:\\FILE") == FA_READONLY);

    CHECK(h.fattribSet("D:\\FILE", 0) == FA_READONLY);
    CHECK(h.modeOf("FILE") == 0644);

    CHECK(h.fattribSet("D:\\FILE", FA_HIDDEN) == 0);            // can't be stored on this drive - no error, like before
    CHECK(h.fattribGet("D:\\FILE") == 0);
}

TEST_CASE("translated disk: read only file can't be opened for writing, overwritten by Fcreate or deleted")
{
    TranslatedDiskHarness h;
    h.touch("RDONLY", 0444);

    CHECK(h.call(GEMDOS_Fopen, {1}, "D:\\RDONLY") == (uint8_t) EACCDN);    // write
    CHECK(h.call(GEMDOS_Fopen, {2}, "D:\\RDONLY") == (uint8_t) EACCDN);    // read / write
    uint8_t handle = h.call(GEMDOS_Fopen, {0}, "D:\\RDONLY");              // read is fine
    CHECK(handle < 0x80);
    h.call(GEMDOS_Fclose, {}, "", handle);

    CHECK(h.call(GEMDOS_Fcreate, {0}, "D:\\RDONLY") == (uint8_t) EACCDN);

    CHECK(h.call(GEMDOS_Fdelete, {}, "D:\\RDONLY") == (uint8_t) EACCDN);   // used to be deleted - dir is writable
    CHECK(access(h.host("RDONLY").c_str(), F_OK) == 0);

    REQUIRE(chmod(h.host("RDONLY").c_str(), 0644) == 0);                    // not read only anymore - can be deleted
    CHECK(h.call(GEMDOS_Fdelete, {}, "D:\\RDONLY") == E_OK);
}

TEST_CASE("translated disk: Fcreate with read only attribute - file is read only, but the returned handle works")
{
    TranslatedDiskHarness h;

    uint8_t handle = h.call(GEMDOS_Fcreate, {FA_READONLY}, "D:\\NEWFILE");
    CHECK(handle < 0x80);                                       // got handle (on FAT, reopening used to fail for non-root)
    CHECK(h.modeOf("NEWFILE") == (h.modeOf("NEWFILE") & ~0222));
    CHECK(h.fattribGet("D:\\NEWFILE") == FA_READONLY);
    h.call(GEMDOS_Fclose, {}, "", handle);
}

TEST_CASE("translated disk: file created on the host while the dir is cached - open, attributes, delete by its 8.3 name")
{
    TranslatedDiskHarness h;
    h.touch("OTHER.TXT");
    CHECK(h.fattribGet("D:\\OTHER.TXT") == 0);                 // the drive's root dir is cached now

    h.touch("Game.prg");                                        // e.g. saved by a Mac app - not in the cache

    uint8_t handle = h.call(GEMDOS_Fopen, {0}, "D:\\GAME.PRG");
    CHECK(handle < 0x80);
    h.call(GEMDOS_Fclose, {}, "", handle);

    CHECK(h.fattribGet("D:\\GAME.PRG") == 0);

    // what Fsfirst / Fsnext list - on a case-insensitive filesystem (APFS) it used to be 'GAME~1.PRG', as opening
    // 'GAME.PRG' had made the name from the ST the long name of the short name 'GAME.PRG'
    std::vector<std::string> names = h.list();
    CHECK(std::count(names.begin(), names.end(), "GAME.PRG") == 1);
    CHECK(std::count(names.begin(), names.end(), "GAME~1.PRG") == 0);

    CHECK(h.call(GEMDOS_Fdelete, {}, "D:\\GAME.PRG") == E_OK);
    CHECK(access(h.host("Game.prg").c_str(), F_OK) != 0);       // the real file is gone
}

TEST_CASE("translated disk: lower case path from the ST - every part of it is found, like on TOS")
{
    TranslatedDiskHarness h;
    REQUIRE(mkdir(h.host("Sub").c_str(), 0755) == 0);
    h.touch("Sub/File.txt");

    uint8_t handle = h.call(GEMDOS_Fopen, {0}, "D:\\sub\\file.txt");    // used to fail on Linux - only the last part was looked up in other case
    CHECK(handle < 0x80);
    h.call(GEMDOS_Fclose, {}, "", handle);

    CHECK(h.call(GEMDOS_Fdelete, {}, "D:\\sub\\file.txt") == E_OK);
    CHECK(access(h.host("Sub/File.txt").c_str(), F_OK) != 0);
}

// names in a host dir exactly as stored - on a case-insensitive filesystem access() can't tell 'A.TXT' from 'a.txt'
static std::vector<std::string> hostNames(const std::string& dirPath)
{
    std::vector<std::string> names;
    DIR* dir = opendir(dirPath.c_str());
    REQUIRE(dir != NULL);
    while(struct dirent* de = readdir(dir)) {
        std::string n = de->d_name;
        if(n != "." && n != "..") {
            names.push_back(n);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

static uint8_t frename(TranslatedDiskHarness& h, const std::string& from, const std::string& to)
{
    std::string both = from + std::string(1, '\0') + to;       // the ST sends both names, each ending with 0
    return h.call(GEMDOS_Frename, {}, both);
}

TEST_CASE("translated disk: Frename doesn't replace an existing file or dir")
{
    TranslatedDiskHarness h;
    h.touch("A.TXT");
    h.touch("B.TXT");
    REQUIRE(system(("echo keep > '" + h.host("B.TXT") + "'").c_str()) == 0);
    REQUIRE(mkdir(h.host("DIR").c_str(), 0755) == 0);

    CHECK(frename(h, "D:\\A.TXT", "D:\\B.TXT") == (uint8_t) EACCDN);     // used to replace B.TXT
    CHECK(frename(h, "D:\\A.TXT", "D:\\DIR") == (uint8_t) EACCDN);
    CHECK(hostNames(h.drive) == std::vector<std::string>({"A.TXT", "B.TXT", "DIR"}));

    FILE* f = fopen(h.host("B.TXT").c_str(), "r");
    REQUIRE(f != NULL);
    char buf[8] = {0};
    CHECK(fgets(buf, sizeof(buf), f) != NULL);
    fclose(f);
    CHECK(std::string(buf) == "keep\n");

    CHECK(frename(h, "D:\\A.TXT", "D:\\C.TXT") == E_OK);                 // new name - fine
    CHECK(hostNames(h.drive) == std::vector<std::string>({"B.TXT", "C.TXT", "DIR"}));
}

TEST_CASE("translated disk: Frename to the same name in other case - renamed, also on a case-insensitive filesystem")
{
    TranslatedDiskHarness h;
    h.touch("LongerName.txt");                                  // short name LONGER~1.TXT

    // e.g. MiNT with long names: on APFS 'LONGERNAME.TXT' already exists - it's the same file, not another one
    CHECK(frename(h, "D:\\LONGER~1.TXT", "D:\\LONGERNAME.TXT") == E_OK);
    CHECK(hostNames(h.drive) == std::vector<std::string>({"LONGERNAME.TXT"}));
}
