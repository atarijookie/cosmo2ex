/*
    Tests of Settings - settings are stored as one file per key in SETTINGS_DIR (or its subdir for per-device prefix)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         // mkdtemp() - macOS has it here, glibc in stdlib.h
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <dirent.h>
#include <sys/stat.h>

#include "doctest.h"

#include "misc/settings.h"
#include "misc/global.h"
#include "misc/utils.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

// creates empty temp dir and points SETTINGS_DIR to it, removes it at the end of test
struct SettingsDir {
    std::string dir;

    SettingsDir() {
        char tmpl[] = "/tmp/ce_test_settings_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        dotEnv["SETTINGS_DIR"] = dir;
    }

    ~SettingsDir() {
        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }

    void writeFile(const std::string& relPath, const char* content) {
        std::string path = dir + "/" + relPath;
        FILE* f = fopen(path.c_str(), "w");
        REQUIRE(f != NULL);
        fputs(content, f);
        fclose(f);
    }
};

TEST_CASE_FIXTURE(SettingsDir, "new device prefix dir gets default values")
{
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    Settings s(mac);
    CHECK(Utils::fileExists(dir + "/001122334455/DRIVELETTER_CONFDRIVE"));
    CHECK(s.getChar("DRIVELETTER_CONFDRIVE", 'C') == 'O');                  // stored default, not the getter default
    CHECK(s.getInt("ACSI_DEVTYPE_1", DEVTYPE_OFF) == DEVTYPE_TRANSLATED);
    CHECK(s.getPath("PATH_GEM_C", "") == Settings::defaultTranslatedDrivePath());         // stored as the web UI shows it (Windows path on Windows)
    CHECK(s.getString("PATH_GEM_C", "") == Utils::pathToUi(Settings::defaultTranslatedDrivePath()));
#if defined(__linux__)
    CHECK(Settings::defaultTranslatedDrivePath() == "/mnt/");                // Linux default stays as it was
#endif

    SUBCASE("defaults are not stored again over changed values") {
        s.setChar("DRIVELETTER_CONFDRIVE", 'P');

        Settings s2(mac);       // dir exists now
        CHECK(s2.getChar("DRIVELETTER_CONFDRIVE", 'C') == 'P');
    }

    SUBCASE("other device has its own settings") {
        s.setChar("DRIVELETTER_CONFDRIVE", 'P');

        uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        Settings s2(mac2);
        CHECK(s2.getChar("DRIVELETTER_CONFDRIVE", 'C') == 'O');
    }
}

TEST_CASE_FIXTURE(SettingsDir, "values round trip")
{
    Settings s;

    s.setString("STR", "/some/path");
    CHECK(std::string(s.getString("STR", "def")) == "/some/path");

    s.setInt("INT", -42);
    CHECK(s.getInt("INT", 0) == -42);

    s.setBool("BOOL", true);
    CHECK(s.getBool("BOOL", false) == true);

    uint8_t bin[3] = {0x01, 0xAB, 0xFF};
    s.setBinaryString("BIN", bin, 3);
    CHECK(memcmp(s.getBinaryString("BIN", 3), bin, 3) == 0);
}

TEST_CASE_FIXTURE(SettingsDir, "getString")
{
    Settings s;

    SUBCASE("missing key returns default") {
        CHECK(std::string(s.getString("MISSING", "def")) == "def");
    }

    SUBCASE("trailing new line from hand edited file is removed") {
        writeFile("EDITED", "/mnt/disk\n");
        CHECK(std::string(s.getString("EDITED", "def")) == "/mnt/disk");

        writeFile("EDITED_CRLF", "/mnt/disk\r\n");
        CHECK(std::string(s.getString("EDITED_CRLF", "def")) == "/mnt/disk");
    }

    SUBCASE("too long default value is cut, not overflowing the buffer") {
        std::string longDef(1000, 'x');
        std::string got = s.getString("MISSING", longDef.c_str());
        CHECK(got == std::string(255, 'x'));
    }
}

TEST_CASE_FIXTURE(SettingsDir, "getBinaryString with too short stored value returns zeros")
{
    Settings s;
    s.setString("SHORT", "ABCD");       // only 2 bytes

    const char* got = s.getBinaryString("SHORT", 6);
    const char zeros[6] = {0};
    CHECK(memcmp(got, zeros, 6) == 0);
}

//-------------------------------------------------------------------------------------------------
// stores ACSI_DEVTYPE_0 .. 7 for device without prefix
static void setAcsiDevTypes(Settings& s, const int types[8])
{
    char key[32];
    for(int id=0; id<8; id++) {
        snprintf(key, sizeof(key), "ACSI_DEVTYPE_%d", id);
        s.setInt(key, types[id]);
    }
}

TEST_CASE_FIXTURE(SettingsDir, "loadAcsiIDs - CE_DD boot ID")
{
    Settings s;
    AcsiIDinfo aii;
    memset(&aii, 0x55, sizeof(aii));        // garbage, so we know loadAcsiIDs() sets everything

    SUBCASE("TRANSLATED ID is the boot ID") {
        const int types[8] = {DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == 3);
        CHECK(aii.enabledIDbits == ((1 << 0) | (1 << 3)));
        CHECK(aii.gotDevTypeRaw);
        CHECK(aii.gotDevTypeTranslated);
    }

    SUBCASE("more TRANSLATED IDs - the first one is the boot ID") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == 2);
    }

    SUBCASE("no TRANSLATED ID and ID 1 not used - ID 1 is the boot ID and is enabled") {
        const int types[8] = {DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == CEDD_DEFAULT_ACSI_ID);
        CHECK(aii.enabledIDbits == ((1 << 0) | (1 << 1) | (1 << 5)));
        CHECK(aii.acsiIDdevType[1] == DEVTYPE_OFF);         // settings are not changed
        CHECK_FALSE(aii.gotDevTypeTranslated);
        CHECK(s.getInt("ACSI_DEVTYPE_1", -1) == DEVTYPE_OFF);
    }

    SUBCASE("no TRANSLATED ID and ID 1 used by RAW - no boot ID") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == ACSI_ID_NONE);
        CHECK(aii.enabledIDbits == (1 << 1));
    }

    SUBCASE("nothing enabled - defaults are stored, ID 1 is TRANSLATED") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == 1);
        CHECK(aii.acsiIDdevType[1] == DEVTYPE_TRANSLATED);
        CHECK(aii.enabledIDbits == (1 << 1));
        CHECK(s.getInt("ACSI_DEVTYPE_1", -1) == DEVTYPE_TRANSLATED);
    }
}

TEST_CASE_FIXTURE(SettingsDir, "loadAcsiIDs - old SD type and unknown types are OFF")
{
    Settings s;
    AcsiIDinfo aii;

    SUBCASE("SD ID next to RAW is reported as OFF and not enabled") {
        const int types[8] = {DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_SD_UNUSED, DEVTYPE_OFF, 7, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.acsiIDdevType[3] == DEVTYPE_OFF);     // TEST_GET_ACSI_IDS sends this - not-set value, not SD
        CHECK(aii.acsiIDdevType[5] == DEVTYPE_OFF);     // unknown type
        CHECK(aii.enabledIDbits == ((1 << 0) | (1 << 1)));  // RAW on 0, default CE_DD boot ID 1; not 3 or 5
        CHECK(aii.ceddId == CEDD_DEFAULT_ACSI_ID);
        CHECK(s.getInt("ACSI_DEVTYPE_3", -1) == DEVTYPE_SD_UNUSED);  // stored settings are not changed
    }

    SUBCASE("SD on ID 1 doesn't block default CE_DD boot ID") {
        const int types[8] = {DEVTYPE_RAW, DEVTYPE_SD_UNUSED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.ceddId == CEDD_DEFAULT_ACSI_ID);
        CHECK(aii.acsiIDdevType[1] == DEVTYPE_OFF);
    }

    SUBCASE("only SD IDs - treated as nothing enabled, defaults are stored") {
        const int types[8] = {DEVTYPE_SD_UNUSED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        setAcsiDevTypes(s, types);
        s.loadAcsiIDs(&aii);

        CHECK(aii.acsiIDdevType[0] == DEVTYPE_OFF);
        CHECK(aii.acsiIDdevType[1] == DEVTYPE_TRANSLATED);
        CHECK(aii.ceddId == 1);
        CHECK(aii.enabledIDbits == (1 << 1));
    }
}

TEST_CASE_FIXTURE(SettingsDir, "settings dir is created on write, not on read")
{
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::string devDir = dir + "/001122334455";

    Settings s(mac);                                    // creates dir with defaults
    REQUIRE(Utils::fileExists(devDir + "/DRIVELETTER_CONFDRIVE"));

    std::string cmd = "rm -rf '" + devDir + "'";        // dir removed while the object exists
    REQUIRE(system(cmd.c_str()) == 0);

    CHECK(s.getInt("SOME_KEY", 123) == 123);            // read of missing dir returns default...
    CHECK_FALSE(Utils::dirExists(devDir));              // ...and doesn't create the dir

    s.setInt("SOME_KEY", 456);                          // write creates the dir and stores the value
    CHECK(Utils::dirExists(devDir));
    CHECK(s.getInt("SOME_KEY", 123) == 456);
}

TEST_CASE_FIXTURE(SettingsDir, "getString is safe to call from multiple threads")
{
    // FDD and HDD cores run in different threads and both read settings - each call must return its own value
    Settings s;
    s.setString("THREAD_A", "/path/for/thread/a");
    s.setString("THREAD_B", "/another/longer/path/for/thread/b");

    std::atomic<int> mismatches(0);

    auto reader = [&mismatches](const char *key, const char *expected) {
        Settings ts;
        for(int i=0; i<2000; i++) {
            if(std::string(ts.getString(key, "def")) != expected) {
                mismatches++;
            }
        }
    };

    std::thread a(reader, "THREAD_A", "/path/for/thread/a");
    std::thread b(reader, "THREAD_B", "/another/longer/path/for/thread/b");
    a.join();
    b.join();

    CHECK(mismatches.load() == 0);
}

TEST_CASE_FIXTURE(SettingsDir, "settings: reader never sees an empty or partial value while it is being written")
{
    // writes used to truncate the file and then write it - a reader at that moment got the default value
    Settings s;
    std::string valueA = "/mnt/disk/images/" + std::string(200, 'a') + ".st";
    std::string valueB = "/mnt/disk/images/" + std::string(150, 'b') + ".msa";
    s.setString("FLOPPY_IMAGE", valueA.c_str());

    std::atomic<bool> done(false);
    std::atomic<int> bad(0), reads(0);
    std::string firstBad;                               // what was read instead - written only by the reader thread

    std::thread reader([&]() {
        Settings rs;
        while(!done) {
            std::string v = rs.getString("FLOPPY_IMAGE", "DEFAULT");
            if(v != valueA && v != valueB) {
                if(bad++ == 0) {
                    firstBad = v.substr(0, 40) + " (" + std::to_string(v.length()) + " chars)";
                }
            }
            reads++;
        }
    });

    for(int i=0; i<300; i++) {
        s.setString("FLOPPY_IMAGE", (i & 1) ? valueA.c_str() : valueB.c_str());
    }
    done = true;
    reader.join();

    CHECK(reads.load() > 0);
    CAPTURE(firstBad);                                  // e.g. DEFAULT - file wasn't there, empty - file was empty
    CHECK(bad.load() == 0);
}

TEST_CASE_FIXTURE(SettingsDir, "atomicWriteInProgress - true only while a temp file of that file exists")
{
    std::string file = dir + "/SOME_KEY";
    CHECK_FALSE(Utils::atomicWriteInProgress(file));

    FILE *f = fopen((file + ".tmp-123-4").c_str(), "w");                // temp file of writeFileAtomically()
    REQUIRE(f != NULL);
    fclose(f);
    CHECK(Utils::atomicWriteInProgress(file));
    CHECK_FALSE(Utils::atomicWriteInProgress(dir + "/SOME"));           // other key, which is a prefix of this one

    unlink((file + ".tmp-123-4").c_str());
    CHECK_FALSE(Utils::atomicWriteInProgress(file));
}

TEST_CASE_FIXTURE(SettingsDir, "settings: atomic write leaves no temp files and keeps permissions")
{
    Settings s;
    s.setInt("SOME_INT", 42);
    s.setChar("SOME_CHAR", 'X');
    s.setBool("SOME_BOOL", true);
    s.setFloat("SOME_FLOAT", 1.5f);

    CHECK(s.getInt("SOME_INT", 0) == 42);
    CHECK(s.getChar("SOME_CHAR", '?') == 'X');
    CHECK(s.getBool("SOME_BOOL", false) == true);
    CHECK(s.getFloat("SOME_FLOAT", 0) == doctest::Approx(1.5));

    mode_t mask = umask(0);
    umask(mask);
    struct stat st;
    REQUIRE(stat((dir + "/SOME_INT").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == (0666 & ~mask));           // new setting: normal permissions, not mkstemp's 0600

    REQUIRE(chmod((dir + "/SOME_INT").c_str(), 0640) == 0);
    s.setInt("SOME_INT", 43);
    REQUIRE(stat((dir + "/SOME_INT").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);                       // existing setting: permissions kept

    int tmpFiles = 0;
    DIR *d = opendir(dir.c_str());
    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strstr(e->d_name, ".tmp-")) {
            tmpFiles++;
        }
    }
    closedir(d);
    CHECK(tmpFiles == 0);
}

TEST_CASE_FIXTURE(SettingsDir, "getPath / setPath - paths stored as the web UI shows them")
{
    Settings s;
    s.setPath("FLOPPY_IMAGE", "/data/floppy/game.st");
    s.setString("PATH_GEM_D", "/mnt/shared");

#if !defined(__CYGWIN__)
    CHECK(s.getString("FLOPPY_IMAGE", "") == "/data/floppy/game.st");  // same paths for UI and ce_cores here
    CHECK(s.getPath("PATH_GEM_D", "") == "/mnt/shared");
#else
    s.setPath("FLOPPY_IMAGE", "/cygdrive/c/data/floppy/game.st");
    CHECK(s.getString("FLOPPY_IMAGE", "") == "C:\\data\\floppy\\game.st");      // what the web UI shows
    s.setString("PATH_GEM_D", "C:\\Atari\\hdd");                       // as typed in the web UI
    CHECK(s.getPath("PATH_GEM_D", "") == "/cygdrive/c/Atari/hdd");
#endif
    CHECK(s.getPath("NOT_THERE", "") == "");
}
