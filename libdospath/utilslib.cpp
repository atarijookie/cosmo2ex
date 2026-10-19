// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <libgen.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdarg.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#include <linux/msdos_fs.h>     // FAT_IOCTL_GET_ATTRIBUTES, FAT_IOCTL_SET_ATTRIBUTES
#endif

#include "utilslib.h"
#include "libdospath.h"

int logLevelConsole = 0;   // disabled by default
int logLevelFile = 0;      // disabled by default
std::string logFilePath = "/tmp/libdospath.log";        // default log file

uint32_t UtilsLib::getCurrentMs(void)
{
    struct timespec tp;
    int res;

    res = clock_gettime(CLOCK_MONOTONIC, &tp);                  // get current time

    if(res != 0) {                                              // if failed, fail
        return 0;
    }

    uint32_t val = (tp.tv_sec * 1000) + (tp.tv_nsec / 1000000);    // convert to milli seconds
    return val;
}

// FAT attributes - only Linux has a way to get / set them (vfat ioctl)
enum { FAT_OK, FAT_NOT_FAT, FAT_ERROR };

// FAT attribute bits - fixed by the FAT format (ATTR_* in linux/msdos_fs.h), here for systems without that header
#define FAT_ATTR_RO         0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYS        0x04
#define FAT_ATTR_ARCH       0x20

static int fatAttributesGet(const std::string &hostPath, uint32_t &dosattrs)
{
#if defined(__linux__)
    int fd = open(hostPath.c_str(), O_RDONLY | O_NONBLOCK);     // O_RDONLY works for dirs too
    if(fd < 0) {
        return FAT_ERROR;
    }

    __u32 attrs = 0;
    int res = ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs);      // fails if the file is not on FAT
    close(fd);

    dosattrs = attrs;
    return (res == 0) ? FAT_OK : FAT_NOT_FAT;
#else
    return FAT_NOT_FAT;
#endif
}

static int fatAttributesSet(const std::string &hostPath, uint8_t attribs)
{
#if defined(__linux__)
    int fd = open(hostPath.c_str(), O_RDONLY | O_NONBLOCK);     // setting doesn't need write access to the fd
    if(fd < 0) {
        return FAT_ERROR;
    }

    __u32 attrs = 0;
    if(ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs) < 0) {       // not on FAT
        close(fd);
        return FAT_NOT_FAT;
    }

    // only these - the kernel takes care of ATTR_DIR, and ATTR_VOLUME on a file would turn it into a volume label
    attrs = 0;
    if(attribs & FA_READONLY)   attrs |= FAT_ATTR_RO;
    if(attribs & FA_HIDDEN)     attrs |= FAT_ATTR_HIDDEN;
    if(attribs & FA_SYSTEM)     attrs |= FAT_ATTR_SYS;
    if(attribs & FA_ARCHIVE)    attrs |= FAT_ATTR_ARCH;

    int res = ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attrs);
    int err = errno;
    close(fd);
    errno = err;
    return (res == 0) ? FAT_OK : FAT_ERROR;
#else
    return FAT_NOT_FAT;
#endif
}

uint8_t UtilsLib::hostAttributes(const std::string &hostPath, const struct stat &st, bool &tryFat)
{
    uint8_t attribs = 0;
    bool isDir = S_ISDIR(st.st_mode);

    if(isDir) {
        attribs |= FA_DIR;
    }

    if(!(st.st_mode & S_IWUSR)) {           // no write permission (on FAT the mode comes from its read only attribute)
        attribs |= FA_READONLY;
    }

    std::string path = hostPath;
    removeTrailingSeparator(path);
    size_t sep = path.rfind(HOSTPATH_SEPAR_CHAR);
    std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);

    if(name.length() > 1 && name[0] == '.' && name != "..") {   // Mac / Unix convention of hiding files starting with '.' ('.' and '..' are not hidden)
        attribs |= FA_HIDDEN;
    }

    if(tryFat && (isDir || S_ISREG(st.st_mode))) {   // not on devices, FIFOs - open() could block or have side effects
        uint32_t dosattrs = 0;
        int res = fatAttributesGet(hostPath, dosattrs);

        if(res == FAT_OK) {
            if(dosattrs & FAT_ATTR_RO)      attribs |= FA_READONLY;
            if(dosattrs & FAT_ATTR_HIDDEN)  attribs |= FA_HIDDEN;
            if(dosattrs & FAT_ATTR_SYS)     attribs |= FA_SYSTEM;
        } else if(res == FAT_NOT_FAT) {
            tryFat = false;
        }
    }

    return attribs;
}

bool UtilsLib::setHostAttributes(const std::string &hostPath, uint8_t attribs)
{
    struct stat st;
    if(stat(hostPath.c_str(), &st) != 0) {
        return false;
    }

    if(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
        int res = fatAttributesSet(hostPath, attribs);

        if(res == FAT_OK) {
            return true;
        }

        if(res == FAT_ERROR) {
            return false;
        }
    }

    // not on FAT: read only is the write permission, hidden (name starting with '.') and system are ignored
    mode_t mode    = st.st_mode & 07777;
    mode_t newMode = (attribs & FA_READONLY) ? (mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)) : (mode | S_IWUSR);

    if(newMode == mode) {                   // nothing to change
        return true;
    }

    return chmod(hostPath.c_str(), newMode) == 0;
}

uint16_t UtilsLib::fileTimeToAtariDate(struct tm *ptm)
{
    uint16_t atariDate = 0;

    if(ptm == NULL) {
        return 0;
    }

    atariDate |= (ptm->tm_year - 80) << 9;            // year (tm_year is 'years since 1900', we want 'years since 1980', so the difference is -80
    atariDate |= (ptm->tm_mon  +  1) << 5;            // month
    atariDate |= (ptm->tm_mday     );                 // day

    return atariDate;
}

uint16_t UtilsLib::fileTimeToAtariTime(struct tm *ptm)
{
    uint16_t atariTime = 0;

    if(ptm == NULL) {
        return 0;
    }

    atariTime |= (ptm->tm_hour      ) << 11;        // hours
    atariTime |= (ptm->tm_min       ) << 5;         // minutes
    atariTime |= (ptm->tm_sec   / 2 );              // seconds

    return atariTime;
}

void UtilsLib::fileDateTimeToHostTime(uint16_t atariDate, uint16_t atariTime, struct tm *ptm)
{
    uint16_t year, month, day;
    uint16_t hours, minutes, seconds;

    year    = (atariDate >> 9)   + 1980; // 0-119 with 0=1980
    month   = (atariDate >> 5)   & 0x0f; // 1-12
    day     =  atariDate         & 0x1f; // 1-31

    hours   =  (atariTime >> 11) & 0x1f;    // 0-23
    minutes =  (atariTime >>  5) & 0x3f;    // 0-59
    seconds = ( atariTime        & 0x1f) * 2;   // in unit of two

    memset(ptm, 0, sizeof(struct tm));
    ptm->tm_year    = year - 1900;      // number of years since 1900.
    ptm->tm_mon     = month - 1;    // The number of months since January, in the range 0 to 11
    ptm->tm_mday    = day;      // The day of the month, in the range 1 to 31.

    ptm->tm_hour    = hours;    // The number of hours past midnight, in the range 0 to 23
    ptm->tm_min     = minutes;  // The number of minutes after the hour, in the range 0 to 59
    ptm->tm_sec     = seconds;  // The number of seconds after the minute, normally in the range 0 to 59,
                                //  but can be up to 60 to allow for leap seconds.
}

void UtilsLib::mergeHostPaths(std::string &dest, const std::string &tail)
{
    if(dest.empty()) {      // if the 1st part is empty, then result is just the 2nd part
        dest = tail;
        return;
    }

    if(tail.empty()) {      // if the 2nd part is empty, don't do anything
        return;
    }

    bool endsWithSepar      = (dest[dest.length() - 1] == HOSTPATH_SEPAR_CHAR);
    bool startsWithSepar    = (tail[0] == HOSTPATH_SEPAR_CHAR);

    if(!endsWithSepar && !startsWithSepar){     // both don't have separator char? add it between them
        dest = dest + HOSTPATH_SEPAR_STRING + tail;
        return;
    }

    if(endsWithSepar && startsWithSepar) {      // both have separator char? remove one
        dest[dest.length() - 1] = 0;
        dest = dest + tail;
        return;
    }

    // in this case one of them has separator, so just merge them together
    dest = dest + tail;
}

void UtilsLib::splitFilenameFromPath(const std::string &pathAndFile, std::string &path, std::string &file)
{
    splitToTwoByDelim(pathAndFile, path, file, HOSTPATH_SEPAR_CHAR);
}

void UtilsLib::splitFilenameFromExt(const std::string &filenameAndExt, std::string &filename, std::string &ext)
{
    splitToTwoByDelim(filenameAndExt, filename, ext, '.');
}

void UtilsLib::splitToTwoByDelim(const std::string &input, std::string &beforeDelim, std::string &afterDelim, char delim)
{
    size_t sepPos = input.rfind(delim);

    if(sepPos == std::string::npos) {                           // delimiter not found? everything belongs to beforeDelim
        beforeDelim = input;
        afterDelim.clear();
    } else {                                                    // delimiter found? split it to two parts
        beforeDelim = input.substr(0, sepPos);
        afterDelim = input.substr(sepPos + 1);
    }
}

void UtilsLib::splitString(const std::string &s, char delim, std::vector<std::string> &elems)
{
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if(item.empty()) {              // skip empty items, e.g. start of path
            continue;
        }

        elems.push_back(item);
    }
}

void UtilsLib::joinStrings(std::vector<std::string>& elems, std::string& output, int count)
{
    // join string using path delimiter, but only 'count' parts of the input
    // if count == -1, join all parts
    // if count >= 0, join only that many parts

    output = "";                    // start with empty string

    int len = elems.size();         // get count of parts we were supplied with

    if(count >= 0 && count < len) { // if count is positive number and it's less than all the parts count, use it
        len = count;
    }

    if(count == 0) {                // if should join 0 elements, return root path only
        output = "/";
        return;
    }

    for(int i=0; i<len; i++) {      // join all or just requested count of parts
        output.append("/");         // append delimiter and element
        output.append(elems[i]);
    }
}

const char *UtilsLib::getExtension(const char *fileName)
{
    const char *pExt;
    pExt = strrchr(fileName, '.');  // find last '.'

    if(pExt == NULL) {              // last '.' not found? skip it
        return NULL;
    }

    pExt++;                         // move beyond '.'
    return pExt;
}

// Does dirPath contain an item with exactly this name? On a case-insensitive filesystem (APFS / HFS+ on macOS, FAT,
// NTFS) access() finds also the same name in other case, e.g. 'GAME.PRG' for 'Game.prg' - then the dir is searched
// for the exact name.
bool UtilsLib::existsWithExactName(const std::string &dirPath, const std::string &name)
{
    std::string fullPath = dirPath;
    mergeHostPaths(fullPath, name);

    if(access(fullPath.c_str(), F_OK) != 0) {       // doesn't exist in any case? (the usual answer, e.g. for new files)
        return false;
    }

    DIR *dir = opendir(dirPath.empty() ? "/" : dirPath.c_str());
    if(dir == NULL) {
        return false;
    }

    bool found = false;
    struct dirent *de;
    while((de = readdir(dir)) != NULL) {
        if(name == de->d_name) {
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

void UtilsLib::out(int logLevel, const char *format, ...)
{
    // NOTE: va_list is consumed by each vprintf() / vfprintf(), so every output gets its own copy of the args
    va_list args, argsCopy;
    va_start(args, format);

    // log to console
    if(logLevel <= logLevelConsole) {   // log level equal or below allowed console value? log to console
        va_copy(argsCopy, args);
        vprintf(format, argsCopy);
        va_end(argsCopy);
        printf("\n");
    }

    // log to file
    if(logLevel <= logLevelFile) {      // log level equal or below allowed file value? log to file
        logRotateIfNeeded();

        FILE *f = fopen(logFilePath.c_str(), "a+t");        // try to open the file

        va_copy(argsCopy, args);
        if(f) {                         // file opened? write to file
            vfprintf(f, format, argsCopy);
            fprintf(f, "\n");
            fclose(f);
        } else {                        // failed to open the file? write to console
            vprintf(format, argsCopy);
            printf("\n");
        }
        va_end(argsCopy);
    }

    va_end(args);
}

void UtilsLib::logRotateIfNeeded(void)
{
    struct stat attr;
    int res = stat(logFilePath.c_str(), &attr);                 // get file stat

    if(res == 0 && (attr.st_size >= (1024*1024))) {             // file too big?
        std::string logFilePathOld = logFilePath + ".1";        // construct old log filename
        printf("will rotate log file: %s -> %s\n", logFilePath.c_str(), logFilePathOld.c_str());
        unlink(logFilePathOld.c_str());                         // if some previous old file exist, remove it
        rename(logFilePath.c_str(), logFilePathOld.c_str());            // rename current to old
    }
}

void UtilsLib::toHostSeparators(std::string &path)
{
    int len = path.length();

    for(int i=0; i<len; i++) {
        if(path[i] == '\\') {
            path[i] = '/';
        }
    }
}

void UtilsLib::removeTrailingSeparator(std::string& path)
{
    int len = path.length();

    if(len > 1 && path[len - 1] == HOSTPATH_SEPAR_CHAR) {    // path ending with '/'? remove it
        path.pop_back();
    }
}
