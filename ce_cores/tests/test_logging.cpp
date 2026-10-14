/*
    Tests of log files - kept open between lines, rotated by size, reopened when deleted / moved from outside
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <map>
#include <string>

#include "doctest.h"

#include "misc/debug.h"
#include "misc/global.h"
#include "misc/utils.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp
extern int logLevel;
extern uint8_t g_outToConsole;

static long fileSize(const std::string& path)
{
    struct stat st;
    return (stat(path.c_str(), &st) == 0) ? (long) st.st_size : -1;
}

static std::string fileContent(const std::string& path)
{
    std::string content;
    FILE* f = fopen(path.c_str(), "r");
    if(f) {
        char bfr[4096];
        size_t got;
        while((got = fread(bfr, 1, sizeof(bfr), f)) > 0) {
            content.append(bfr, got);
        }
        fclose(f);
    }
    return content;
}

static int countLines(const std::string& s)
{
    int cnt = 0;
    for(size_t i=0; i<s.length(); i++) {
        if(s[i] == '\n') {
            cnt++;
        }
    }
    return cnt;
}

// creates empty temp dir, points LOG_DIR to it and enables debug logging; restores everything at the end of test
struct LogDir {
    std::string dir;
    int oldLogLevel;

    LogDir() {
        char tmpl[] = "/tmp/ce_test_log_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;

        useLogDir(dir);
        oldLogLevel = logLevel;
        logLevel = LOG_DEBUG;
        g_outToConsole = 0;
    }

    ~LogDir() {
        logLevel = oldLogLevel;
        Debug::closeLogFiles();
        dotEnv.erase("LOG_DIR");
        for(int i=LOGFILE_DISCOVERY; i<=LOGFILE_IKBD; i++) {
            Debug::getCoreLogFileName(true, i);
        }

        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }

    void useLogDir(const std::string& logDir) {
        dotEnv["LOG_DIR"] = logDir;
        for(int i=LOGFILE_DISCOVERY; i<=LOGFILE_IKBD; i++) {
            Debug::getCoreLogFileName(true, i);     // re-create log file names from LOG_DIR
        }
    }

    std::string fddLog(void) { return dir + "/" CORE_FDD_LOG_FILENAME; }
};

TEST_CASE_FIXTURE(LogDir, "log lines are in the file right after logging, while file stays open")
{
    Debug::out(LOGFILE_FDD, LOG_INFO, "first line %d", 1);
    CHECK(fileContent(fddLog()).find("first line 1") != std::string::npos);    // line buffered - no close needed

    Debug::out(LOGFILE_FDD, LOG_ERROR, "second line %s", "two");
    std::string content = fileContent(fddLog());
    CHECK(content.find("second line two") != std::string::npos);
    CHECK(countLines(content) == 2);

    SUBCASE("each log goes to its own file") {
        Debug::out(LOGFILE_HDD, LOG_INFO, "hdd line");
        CHECK(fileContent(dir + "/" CORE_HDD_LOG_FILENAME).find("hdd line") != std::string::npos);
        CHECK(fileContent(fddLog()).find("hdd line") == std::string::npos);
    }

    SUBCASE("levels above logLevel are not logged") {
        logLevel = LOG_WARNING;
        Debug::out(LOGFILE_FDD, LOG_INFO, "hidden info");
        Debug::out(LOGFILE_FDD, LOG_WARNING, "visible warning");

        content = fileContent(fddLog());
        CHECK(content.find("hidden info") == std::string::npos);
        CHECK(content.find("visible warning") != std::string::npos);
    }

    SUBCASE("outBfr goes to the same open file") {
        uint8_t bfr[20];
        for(int i=0; i<20; i++) {
            bfr[i] = 'A' + i;
        }
        Debug::outBfr(LOGFILE_FDD, bfr, 20);

        content = fileContent(fddLog());
        CHECK(content.find("outBfr - 20 bytes") != std::string::npos);
        CHECK(content.find("41 42 43") != std::string::npos);
        CHECK(content.find("ABCDEFGHIJKLMNOP") != std::string::npos);
    }
}

TEST_CASE_FIXTURE(LogDir, "log file name change (e.g. LOG_DIR from .env) switches to the new file")
{
    Debug::out(LOGFILE_FDD, LOG_INFO, "in old dir");

    std::string dir2 = dir + "/other";
    REQUIRE(mkdir(dir2.c_str(), 0700) == 0);
    useLogDir(dir2);

    Debug::out(LOGFILE_FDD, LOG_INFO, "in new dir");

    CHECK(fileContent(fddLog()).find("in new dir") == std::string::npos);
    CHECK(fileContent(dir2 + "/" CORE_FDD_LOG_FILENAME).find("in new dir") != std::string::npos);
}

TEST_CASE_FIXTURE(LogDir, "log file is rotated when it reaches LOG_ROTATE_SIZE")
{
    std::string line(1000, 'x');

    int linesForRotation = (LOG_ROTATE_SIZE / 1000) + 10;
    for(int i=0; i<linesForRotation; i++) {
        Debug::out(LOGFILE_FDD, LOG_INFO, "%s", line.c_str());
    }

    CHECK(fileSize(fddLog() + ".1") >= LOG_ROTATE_SIZE);    // full file was moved to .1
    CHECK(fileSize(fddLog()) < LOG_ROTATE_SIZE);            // new file contains only lines after rotation
    CHECK(fileSize(fddLog()) > 0);

    SUBCASE("second rotation replaces the old .1 file") {
        Debug::out(LOGFILE_FDD, LOG_INFO, "marker in second file");

        for(int i=0; i<linesForRotation; i++) {
            Debug::out(LOGFILE_FDD, LOG_INFO, "%s", line.c_str());
        }

        CHECK(fileContent(fddLog() + ".1").find("marker in second file") != std::string::npos);
        CHECK(fileSize(fddLog() + ".1") < 2 * LOG_ROTATE_SIZE);
    }
}

TEST_CASE_FIXTURE(LogDir, "existing content of log file counts for rotation")
{
    // file which is almost full before the app starts logging to it
    std::string almostFull(LOG_ROTATE_SIZE - 100, 'y');
    FILE* f = fopen(fddLog().c_str(), "w");
    REQUIRE(f != NULL);
    fwrite(almostFull.c_str(), 1, almostFull.length(), f);
    fclose(f);

    Debug::out(LOGFILE_FDD, LOG_INFO, "%s", std::string(200, 'z').c_str());    // over the limit now
    CHECK(fileSize(fddLog() + ".1") == -1);                                   // rotation happens before next line

    Debug::out(LOGFILE_FDD, LOG_INFO, "after rotation");
    CHECK(fileSize(fddLog() + ".1") > LOG_ROTATE_SIZE);
    CHECK(fileContent(fddLog()).find("after rotation") != std::string::npos);
    CHECK(fileContent(fddLog()).find("yyyy") == std::string::npos);
}

TEST_CASE_FIXTURE(LogDir, "log file deleted or moved from outside is created again")
{
    Debug::out(LOGFILE_FDD, LOG_INFO, "before");

    SUBCASE("deleted") {
        REQUIRE(unlink(fddLog().c_str()) == 0);
    }

    SUBCASE("moved") {
        REQUIRE(rename(fddLog().c_str(), (fddLog() + ".moved").c_str()) == 0);
    }

    Utils::sleepMs(LOG_FILE_CHECK_INTERVAL_MS + 100);       // the check runs at most once per interval
    Debug::out(LOGFILE_FDD, LOG_INFO, "after");

    std::string content = fileContent(fddLog());
    CHECK(content.find("after") != std::string::npos);
    CHECK(content.find("before") == std::string::npos);
}
