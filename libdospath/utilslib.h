#ifndef _UTILS_H_
#define _UTILS_H_

#include <signal.h>
#include <string>
#include <vector>

#include <stdint.h>
#include <sys/stat.h>

extern "C" volatile sig_atomic_t sigintReceived;

#ifndef MIN
    #define MIN(x, y)   (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
    #define MAX(x, y)   (((x) > (y)) ? (x) : (y))
#endif

class UtilsLib {
    friend class TestClass;

public:
    static uint32_t getCurrentMs(void);

    static void toHostSeparators(std::string &path);
    static void removeTrailingSeparator(std::string& path);

    // GEMDOS attributes of host file / dir (st = its stat): FA_DIR, FA_READONLY (owner has no write permission),
    // FA_HIDDEN (name starts with '.'), and on FAT (Linux) also its read only / hidden / system attributes.
    // tryFat: in - try to get FAT attributes, out - false when not on FAT (so it's not tried again for the rest of the dir)
    static uint8_t hostAttributes(const std::string &hostPath, const struct stat &st, bool &tryFat);
    // FA_READONLY is stored as write permission, on FAT (Linux) also hidden / system / archive go to FAT attributes.
    // What the host can't store (e.g. hidden on ext4 - would mean rename) is ignored. False on failure, errno set.
    static bool setHostAttributes(const std::string &hostPath, uint8_t attribs);
    static void fileDateTimeToHostTime(uint16_t atariDate, uint16_t atariTime, struct tm *ptm);
    static uint16_t fileTimeToAtariTime(struct tm *ptm);
    static uint16_t fileTimeToAtariDate(struct tm *ptm);

    static void mergeHostPaths(std::string &dest, const std::string &tail);
    static void splitFilenameFromPath(const std::string &pathAndFile, std::string &path, std::string &file);
    static void splitFilenameFromExt(const std::string &filenameAndExt, std::string &filename, std::string &ext);
    static void splitToTwoByDelim(const std::string &input, std::string &beforeDelim, std::string &afterDelim, char delim);

    static void splitString(const std::string &s, char delim, std::vector<std::string> &elems);
    static void joinStrings(std::vector<std::string> &elems, std::string& output, int count=-1);

    static const char *getExtension(const char *fileName);
    static bool fileExists(std::string &hostPath);

    static void out(int logLevel, const char *format, ...);
    static void logRotateIfNeeded(void);
};

#endif

