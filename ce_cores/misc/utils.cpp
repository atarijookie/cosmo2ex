// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include "misc/utils.h"

#include <libgen.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cctype>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <limits.h>
#include <atomic>
#if defined(__CYGWIN__)
#include <sys/cygwin.h>     // cygwin_create_path()
#endif

//--------
// following includes are here for the code for getting IP address of interfaces
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
//--------

#include "../hdd/translated/translatedhelper.h"
#include "../hdd/translated/gemdos.h"
#include "debug.h"
#include "version.h"
#include "settings.h"
#include "global.h"

std::map<std::string, std::string> dotEnv;

uint32_t Utils::getCurrentMs(void)
{
    struct timespec tp;
    int res;

    res = clock_gettime(CLOCK_MONOTONIC, &tp);                  // get current time

    if(res != 0) {                                              // if failed, fail
        return 0;
    }

    uint32_t val = ((uint32_t) tp.tv_sec * 1000) + (uint32_t) (tp.tv_nsec / 1000000);   // convert to milli seconds - unsigned math, wraps around well-defined
    return val;
}

uint32_t Utils::elapsedMs(uint32_t startMs)
{
    return getCurrentMs() - startMs;    // unsigned subtraction - correct even when ms counter wraps around
}

uint16_t Utils::fileTimeToAtariDate(struct tm *ptm)
{
    uint16_t atariDate = 0;

    if(ptm == NULL) {
        return 0;
    }

    // GEMDOS date can hold only years 1980..2107 (7 bits) - clamp, otherwise e.g. 1970 (common in ZIPs) wraps to 2098
    int year = ptm->tm_year - 80;                     // tm_year is 'years since 1900', we want 'years since 1980', so the difference is -80
    int mon  = ptm->tm_mon + 1;
    int mday = ptm->tm_mday;

    if(year < 0) {                                    // before 1980? use the earliest date: 1980-01-01
        year = 0;
        mon  = 1;
        mday = 1;
    } else if(year > 127) {                           // after 2107? use the latest date: 2107-12-31
        year = 127;
        mon  = 12;
        mday = 31;
    }

    atariDate |= year << 9;                           // year
    atariDate |= mon  << 5;                           // month
    atariDate |= mday;                                // day

    return atariDate;
}

uint16_t Utils::fileTimeToAtariTime(struct tm *ptm)
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

void Utils::fileDateTimeToHostTime(uint16_t atariDate, uint16_t atariTime, struct tm *ptm)
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

std::string Utils::mergeHostPaths3(const std::string& head, const char* tail)
{
    std::string tailStr(tail);
    return Utils::mergeHostPaths2(head, tailStr);
}

std::string Utils::mergeHostPaths2(const std::string& head, const std::string& tail)
{
    // this method creates merged path, and it doesn't modifiy head:    rev_val = head + tail

    std::string retVal = head;              // copy of head into return value, so we won't modify head (not static - this is called from multiple threads)
    Utils::mergeHostPaths(retVal, tail);    // merge retVal (head) with tail
    return retVal;                          // return merged value
}

void Utils::mergeHostPaths(std::string &dest, const std::string &tail)
{
    // this method creates merged path, but it modifies dest:     dest = dest + tail

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
        dest.erase(dest.length() - 1);          // really shorten the string (writing 0 there would leave the 0 inside of the string)
        dest = dest + tail;
        return;
    }

    // in this case one of them has separator, so just merge them together
    dest = dest + tail;
}

void Utils::splitFilenameFromPath(const std::string &pathAndFile, std::string &path, std::string &file)
{
    splitToTwoByDelim(pathAndFile, path, file, HOSTPATH_SEPAR_CHAR);
}

void Utils::splitFilenameFromExt(const std::string &filenameAndExt, std::string &filename, std::string &ext)
{
    splitToTwoByDelim(filenameAndExt, filename, ext, '.');
}

void Utils::splitToTwoByDelim(const std::string &input, std::string &beforeDelim, std::string &afterDelim, char delim)
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

void Utils::mergeFilenameAndExtension(const std::string& shortFn, const std::string& shortExt, bool extendWithSpaces, std::string& merged)
{
    std::string shortFn2 = shortFn, shortExt2 = shortExt;       // make copies so we don't modify originals

    if(shortFn2.size() > 8)             // filename too long? shorten
        shortFn2.resize(8);

    if(shortExt2.size() > 3)            // file ext too long? shorten
        shortExt2.resize(3);

    if(extendWithSpaces) {              // if should extend
        if(shortFn2.size() < 8)         // filename too short? extend
            shortFn2.resize(8, ' ');

        if(shortExt2.size() < 3)        // extension too short? extend
            shortExt2.resize(3, ' ');
    }

    merged = shortFn2;                  // put in the filename

    if(shortExt2.size() > 0)            // if got extension, append extension
        merged += std::string(".") + shortExt2;
}

void Utils::extendWithSpaces(const char *normalFname, char *extendedFn)
{
    std::string sNormalFname = normalFname, fname, ext, sExtendedFn;

    Utils::splitFilenameFromExt(normalFname, fname, ext);               // split 'FILE.C' to 'FILE' and 'C'
    Utils::mergeFilenameAndExtension(fname, ext, true, sExtendedFn);    // extend and merge, so will get 'FILE    .C  '

    strcpy(extendedFn, sExtendedFn.c_str());
}

void Utils::sleepMs(uint32_t ms)
{
    uint32_t us = ms * 1000;

    usleep(us);
}

bool Utils::copyFile(std::string &src, std::string &dst)
{
    FILE *from, *to;

    from = fopen(src.c_str(), "rb");               // open source file

    if(!from) {
        logHdd(LOG_ERROR, "Utils::copyFile - failed to open source file %s", src.c_str());
        return false;
    }

    to = fopen(dst.c_str(), "wb");                 // open destrination file

    if(!to) {
        fclose(from);

        logHdd(LOG_ERROR, "Utils::copyFile - failed to open destination file %s", dst.c_str());
        return false;
    }

    bool res = copyFileByHandles(from, to);

    fclose(from);
    fclose(to);

    return res;
}

bool Utils::copyFile(FILE *from, std::string &dst)
{
    FILE *to;

    to = fopen(dst.c_str(), "wb");                 // open destrination file

    if(!to) {
        logHdd(LOG_ERROR, "Utils::copyFile - failed to open destination file %s", dst.c_str());
        return false;
    }

    bool res = copyFileByHandles(from, to);

    fclose(to);

    return res;
}

bool Utils::copyFileByHandles(FILE *from, FILE *to)
{
    uint8_t bfr64k[64 * 1024];

    while(1) {                                              // copy the file in loop
        size_t read = fread(bfr64k, 1, 64 * 1024, from);

        if(read == 0) {                                     // didn't read anything? quit
            break;
        }

        if(fwrite(bfr64k, 1, read, to) != read) {           // failed to write everything? (e.g. disk full)
            logHdd(LOG_ERROR, "Utils::copyFileByHandles - fwrite() failed: %s", strerror(errno));
            return false;
        }

        if(feof(from)) {                                    // end of file reached? quit
            break;
        }
    }

    if(ferror(from)) {                                      // stopped because of read error, not end of file?
        logHdd(LOG_ERROR, "Utils::copyFileByHandles - fread() failed: %s", strerror(errno));
        return false;
    }

    if(fflush(to) != 0) {                                   // buffered data written? (fclose() result is not checked by callers)
        logHdd(LOG_ERROR, "Utils::copyFileByHandles - fflush() failed: %s", strerror(errno));
        return false;
    }

    return true;
}

void Utils::SWAPWORD(uint16_t &w)
{
    uint16_t a,b;

    a = w >> 8;         // get top
    b = w  & 0xff;      // get bottom

    w = (b << 8) | a;   // store swapped
}

uint16_t Utils::SWAPWORD2(uint16_t w)
{
    uint16_t a,b;

    a = w >> 8;         // get top
    b = w  & 0xff;      // get bottom

    w = (b << 8) | a;   // store swapped
    return w;
}

void Utils::swapWordsBuffer(uint8_t *bfr, uint16_t size)
{
    uint8_t tmp;

    for(uint16_t i=0; i<size; i += 2) {
        tmp = bfr[i];
        bfr[i] = bfr[i+1];
        bfr[i+1] = tmp;
    }
}

void Utils::swapWordsBufferWithCopy(uint8_t *bfrDest, uint8_t *bfrSrc, uint16_t size)
{
    for(uint16_t i=0; i<size; i += 2) {
        bfrDest[i + 1] = bfrSrc[i];
        bfrDest[i    ] = bfrSrc[i + 1];
    }
}

void Utils::getIpAdds(uint8_t *bfrIPs, uint8_t *bfrMasks)
{
    struct ifaddrs *ifaddr, *ifa;
    int family, n;

    memset(bfrIPs, 0, 10);                                          // set to 0 - this means 'not present'

    if(bfrMasks) {
        memset(bfrMasks, 0, 10);                                    // set to 0 - this means 'not present'
    }

    if (getifaddrs(&ifaddr) == -1) {
        return;
    }

    // Walk through linked list, maintaining head pointer so we can free list later
    for (ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        if (family != AF_INET) {                                // not good interface? skip it
            continue;
        }

        sockaddr_in *saiIp  = (sockaddr_in *) ifa->ifa_addr;
        sockaddr_in *saiMsk = (sockaddr_in *) ifa->ifa_netmask;     // can be NULL

        uint32_t ip            = saiIp->sin_addr.s_addr;
        uint32_t mask          = saiMsk ? saiMsk->sin_addr.s_addr : 0;

        uint8_t *pIp   = NULL;
        uint8_t *pMsk  = NULL;

        if(strcmp(ifa->ifa_name,"eth0")==0) {                   // for eth0 - store at offset 0
            pIp = bfrIPs;

            if(bfrMasks) {                                      // got masks buffer? store offset 0
                pMsk = bfrMasks;
            }
        }

        if(strcmp(ifa->ifa_name,"wlan0")==0) {                  // for wlan0 - store at offset 5
            pIp = bfrIPs + 5;

            if(bfrMasks) {                                      // got masks buffer? store offset 5
                pMsk = bfrMasks + 5;
            }
        }

        if(pIp == NULL) {                                       // if not an interface we're interested in, skip it
            continue;
        }

        pIp[0] = 1;                                             // enabled?
        pIp[1] = (uint8_t)  ip;                                    // store the ip
        pIp[2] = (uint8_t) (ip >>  8);
        pIp[3] = (uint8_t) (ip >> 16);
        pIp[4] = (uint8_t) (ip >> 24);

        if(pMsk && saiMsk) {                                    // got mask buffer and interface has mask?
            pMsk[0] = 1;                                        // enabled?
            pMsk[1] = (uint8_t)  mask;                             // store the mask
            pMsk[2] = (uint8_t) (mask >>  8);
            pMsk[3] = (uint8_t) (mask >> 16);
            pMsk[4] = (uint8_t) (mask >> 24);
        }
    }

    freeifaddrs(ifaddr);
}

uint16_t Utils::getWord(uint8_t *bfr)
{
    uint16_t val = 0;

    val = bfr[0];       // get hi
    val = val << 8;

    val |= bfr[1];      // get lo

    return val;
}

uint32_t Utils::getDword(uint8_t *bfr)
{
    uint32_t val = 0;

    val = bfr[0];       // get hi
    val = val << 8;

    val |= bfr[1];      // get mid hi
    val = val << 8;

    val |= bfr[2];      // get mid lo
    val = val << 8;

    val |= bfr[3];      // get lo

    return val;
}

uint32_t Utils::get24bits(uint8_t *bfr)
{
    uint32_t val = 0;

    val  = bfr[0];       // get hi
    val  = val << 8;

    val |= bfr[1];      // get mid
    val  = val << 8;

    val |= bfr[2];      // get lo

    return val;
}

void Utils::storeWord(uint8_t *bfr, uint16_t val)
{
    bfr[0] = val >> 8;  // store hi
    bfr[1] = val;       // store lo
}

void Utils::storeDword(uint8_t *bfr, uint32_t val)
{
    bfr[0] = val >> 24; // store hi
    bfr[1] = val >> 16; // store mid hi
    bfr[2] = val >>  8; // store mid lo
    bfr[3] = val;       // store lo
}

void Utils::store24bits(uint8_t *bfr, uint32_t val)
{
    bfr[0] = val >> 16;
    bfr[1] = val >>  8;
    bfr[2] = val;
}

void Utils::createTimezoneString(char *str)
{
    Settings s;
    float     utcOffset;
    utcOffset = s.getFloat("TIME_UTC_OFFSET", 0);          // read UTC offset from settings

    createTimezoneStringFromOffset(utcOffset, str);
}

void Utils::createTimezoneStringFromOffset(float utcOffset, char *str)
{
    char  signChar;

    if(utcOffset >= 0.0f) {         // is UTC offset positive? (e.g. Berlin is +1 / +2) TZ should have '-' because it's east of Prime Meridian
        signChar = '-';
    } else {                        // is UTC offset negative? (e.g. New York is -5 / -4) TZ should have '+' because it's west of Prime Meridian
        signChar = '+';
    }

    // whole offset in minutes (without sign), rounded - so e.g. 5.75 doesn't become 5:44 because of float precision
    int ofsTotalMinutes = (int) (fabsf(utcOffset) * 60.0f + 0.5f);
    int ofsHours        = ofsTotalMinutes / 60;
    int ofsMinutes      = ofsTotalMinutes % 60;

    char tmp[32];
    if(ofsTotalMinutes == 0) {                                  // no offset?
        snprintf(tmp, sizeof(tmp), "UTC");
    } else if(ofsMinutes != 0) {                                // got offset with minutes? (also when hours are 0, e.g. 0.5)
        snprintf(tmp, sizeof(tmp), "UTC%c%02d:%02d", signChar, ofsHours, ofsMinutes);
    } else {                                                    // got offset - only hours (no minutes)
        snprintf(tmp, sizeof(tmp), "UTC%c%02d", signChar, ofsHours);
    }
    strcpy(str, tmp);
}

void Utils::setTimezoneVariable_inThisContext(void)
{
    char utcOfsset[64];
    createTimezoneString(utcOfsset);

    logHdd(LOG_DEBUG, "Utils::setTimezoneVariable_inThisContext() -- setting TZ variable to: %s\n", utcOfsset);

    setenv("TZ", utcOfsset, 1);
    tzset();                    // apply it now (localtime_r() doesn't call tzset() itself)
}

#if defined(__linux__)

// label of a disk / partition, e.g. "/dev/sda1" -> "MY DISK", from the links in /dev/disk/by-label (made by udev)
std::string Utils::getDeviceLabel(const std::string & devicePath)
{
#define DEV_BY_LABEL_PATH "/dev/disk/by-label/"
    std::string label("");

    if(devicePath.substr(0,5) != "/dev/") return label;
    std::string devShort = devicePath.substr(5);

    DIR *d = opendir(DEV_BY_LABEL_PATH);
    if(d != NULL) {
        struct dirent * de;
        char link_path[288];
        char link_target[256];
        while((de = readdir(d)) != NULL) {
            if(de->d_type == DT_LNK) {  // symbolic link
                snprintf(link_path, sizeof(link_path), "%s%s", DEV_BY_LABEL_PATH, de->d_name);
                ssize_t n = readlink(link_path, link_target, sizeof(link_target) - 1);
                if(n >= 0) {
                    link_target[n] = '\0';
                    char * target_short = strrchr(link_target, '/');
                    if(target_short != NULL) {
                        target_short++;
                        if(devShort == target_short) label = de->d_name;
                    }
                }
            }
        }
        closedir(d);
    }
    return decodeHexEscapes(label);
}

#else

std::string Utils::getDeviceLabel(const std::string & devicePath)
{
    return "";      // no /dev/disk/by-label here - no label
}

#endif

std::string Utils::decodeHexEscapes(const std::string& in)
{
    // labels in /dev/disk/by-label have special chars escaped as \xNN, e.g. space is \x20 - decode them back
    std::string label = in;
    size_t pos = 0;

    while((pos = label.find("\\x", pos)) != std::string::npos) {
        if(pos + 3 < label.length() && isxdigit((unsigned char) label[pos + 2]) && isxdigit((unsigned char) label[pos + 3])) {    // got 2 hex digits after \x?
            int c = (int) strtol(label.substr(pos + 2, 2).c_str(), NULL, 16);
            label.replace(pos, 4, 1, (char) c);
            pos += 1;           // continue after the decoded char
        } else {                // not a valid escape? keep it as it is, continue after it
            pos += 2;
        }
    }

    return label;
}

void Utils::splitString(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
}

// make dir recursively (like mkdir -p)
int Utils::mkpath(const char *dir, int mode)
{
    struct stat sb;
    char * dircopy;

    if (dir == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (stat(dir, &sb) == 0) {
        return 0;   // the directory already exists
    }

    // strdup() dir because some implementations of dirname() store the result in the input buffer
    dircopy = strdup(dir);
    mkpath(dirname(dircopy), mode); // create (recursively) parent directory
    free(dircopy);

    return mkdir(dir, mode);    // create directory itself, once all parents are created
}

const char *Utils::getExtension(const char *fileName)
{
    const char *pExt;
    pExt = strrchr(fileName, '.');  // find last '.'

    if(pExt == NULL) {              // last '.' not found? skip it
        return NULL;
    }

    pExt++;                         // move beyond '.'
    return pExt;
}

void Utils::createPathWithOtherExtension(std::string &inPathWithOriginalExt, const char *otherExtension, std::string &outPathWithOtherExtension)
{
    outPathWithOtherExtension = inPathWithOriginalExt;  // first just copy the input path

    const char *originalExt = Utils::getExtension(inPathWithOriginalExt.c_str());

    if(originalExt == NULL) {                           // failed to find extension? fail
        return;
    }
    int originalExtLen = strlen(originalExt);           // get length of original extension

    if(outPathWithOtherExtension.size() < ((size_t) originalExtLen)) { // path shorter than extension? (how could this happen???) fail
        return;
    }

    outPathWithOtherExtension.resize(outPathWithOtherExtension.size() - originalExtLen);    // remove original extension
    outPathWithOtherExtension = outPathWithOtherExtension + std::string(otherExtension);    // append other extension
}

bool Utils::fileExists(const std::string& path)
{
    return Utils::fileExists(path.c_str());
}

bool Utils::fileExists(const char* path)
{
    struct stat sb;

    // if can get stat and ISREG is in mode, it's a regular file
    if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode)) {
        return true;
    }

    return false;
}

bool Utils::dirExists(std::string& path)
{
    struct stat sb;

    // if can get stat and ISDIR is in mode, it's a dir
    if (stat(path.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        return true;
    }

    return false;
}

bool Utils::devExists(std::string& path)
{
    struct stat sb;

    // if can get stat and S_ISBLK is in mode, it's a block device
    if (stat(path.c_str(), &sb) == 0 && S_ISBLK(sb.st_mode)) {
        return true;
    }

    return false;
}

// without MSG_NOSIGNAL (macOS) the socket itself must be told not to raise SIGPIPE - see SEND_NOSIGNAL
static void socketNoSigpipe(int fd)
{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    if(fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#else
    (void) fd;
#endif
}

int Utils::socketCloexec(int domain, int type, int protocol)
{
#if defined(SOCK_CLOEXEC)       // Linux, BSDs, Cygwin
    int fd = socket(domain, type | SOCK_CLOEXEC, protocol);
#else                           // e.g. macOS - set it right after creating, there's a short window when a fork() would inherit it
    int fd = socket(domain, type, protocol);
    if(fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    socketNoSigpipe(fd);
    return fd;
}

int Utils::acceptCloexec(int fdListen, struct sockaddr *addr, socklen_t *addrLen)
{
#if defined(SOCK_CLOEXEC)       // where there's SOCK_CLOEXEC, there's also accept4()
    int fd = accept4(fdListen, addr, addrLen, SOCK_CLOEXEC);
#else
    int fd = accept(fdListen, addr, addrLen);
    if(fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    socketNoSigpipe(fd);        // accepted socket doesn't inherit it from the listening one

    // On Linux the accepted socket is always blocking, but on Cygwin / BSD / macOS it inherits O_NONBLOCK from the
    // (non-blocking) listening socket - then SO_RCVTIMEO doesn't apply and recv() fails with EAGAIN right away when
    // the data isn't there yet. Make it blocking everywhere, like on Linux; callers which want it non-blocking set it.
    if(fd >= 0) {
        int flags = fcntl(fd, F_GETFL);
        if(flags != -1 && (flags & O_NONBLOCK)) {
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
    }

    return fd;
}

int Utils::bcdToInt(int bcd)
{
    int a,b;

    a = bcd >> 4;       // upper nibble
    b = bcd &  0x0f;    // lower nibble

    return ((a * 10) + b);
}

void Utils::toUpperCaseString(std::string &st)
{
    int i, len;
    len = st.length();

    for(i=0; i<len; i++) {
        st[i] = toupper(st[i]);
    }
}

void Utils::loadDotEnv(void)
{
    bool good = loadDotEnvFrom("./.env");

    dotEnvAddDefaultDirs();     // before substitution, so e.g. ${DATA_DIR}/core_cmd.token works even without DATA_DIR in .env

    if(good) {      // if something was loaded, do the vars subtitution
        dotEnvSubstituteVars();
    }
}

bool Utils::isWindowsPath(const std::string& path)
{
    bool driveLetter = path.length() >= 2 && isalpha((unsigned char) path[0]) && path[1] == ':';    // C:\ or C:/
    bool unc         = path.length() >= 2 && path[0] == '\\' && path[1] == '\\';                  // \\server\share
    return driveLetter || unc;
}

#if defined(__CYGWIN__)
// cygwin_create_path() - cygwin_conv_path() which allocates the result; on failure returns the path unchanged
static std::string cygwinConvertPath(cygwin_conv_path_t what, const std::string& path)
{
    char* converted = (char*) cygwin_create_path(what, path.c_str());

    if(!converted) {
        logHdd(LOG_WARNING, "Utils - converting path '%s' failed : %s", path.c_str(), strerror(errno));
        return path;
    }

    std::string result = converted;
    free(converted);
    return result;
}
#endif

std::string Utils::pathFromUi(const std::string& uiPath)
{
#if defined(__CYGWIN__)
    if(isWindowsPath(uiPath)) {
        std::string posixPath = cygwinConvertPath(CCP_WIN_A_TO_POSIX, uiPath);

        // Cygwin drops the trailing separator (C:\Atari\ -> /cygdrive/c/Atari) - keep it, it marks a dir and paths are
        // merged / compared with it
        char last = uiPath[uiPath.length() - 1];
        if((last == '\\' || last == '/') && !posixPath.empty() && posixPath[posixPath.length() - 1] != '/') {
            posixPath += '/';
        }
        return posixPath;
    }
#endif
    return uiPath;
}

std::string Utils::pathToUi(const std::string& hostPath)
{
#if defined(__CYGWIN__)
    if(!hostPath.empty() && hostPath[0] == '/') {           // absolute POSIX path - relative / empty ones stay as they are
        std::string winPath = cygwinConvertPath(CCP_POSIX_TO_WIN_A, hostPath);

        // Cygwin drops the trailing separator here too (/cygdrive/c/Atari/ -> C:\Atari) - keep it, see pathFromUi()
        if(hostPath[hostPath.length() - 1] == '/' && !winPath.empty() && winPath[winPath.length() - 1] != '\\') {
            winPath += '\\';
        }
        return winPath;
    }
#endif
    return hostPath;
}

std::string Utils::defaultDir(const std::string& key)
{
    /* Linux: system dirs, as the .env shipped for Linux / docker has them.
       macOS: ~/Library/Application Support/cosmo2ex/..., Windows (Cygwin): %LOCALAPPDATA%\cosmo2ex\...
       - desktop users run it as regular user, without access to /var and /run */

    const char* sub = (key == "LOG_DIR") ? "log" : ((key == "DATA_DIR") ? "data" : ((key == "PID_DIR") ? "run" : NULL));

    if(!sub) {
        return "";
    }

#if defined(__APPLE__)
    const char* home = getenv("HOME");

    if(home && home[0]) {
        return std::string(home) + "/Library/Application Support/cosmo2ex/" + sub;
    }
#elif defined(__CYGWIN__)
    const char* localAppData = getenv("LOCALAPPDATA");      // Windows path, e.g. C:\Users\x\AppData\Local

    if(localAppData && localAppData[0]) {
        char* posixPath = (char*) cygwin_create_path(CCP_WIN_A_TO_POSIX, localAppData);   // -> /cygdrive/c/Users/x/AppData/Local

        if(posixPath) {
            std::string dir = std::string(posixPath) + "/cosmo2ex/" + sub;
            free(posixPath);
            return dir;
        }
    }
#endif

    // Linux - and fallback if the per-user dir can't be found on other OS
    if(key == "LOG_DIR") {
        return "/var/log/ce";
    }

    if(key == "DATA_DIR") {
        return "/var/lib/ce";
    }

    return "/run/ce";
}

void Utils::dotEnvAddDefaultDirs(void)
{
    const char* keys[] = {"LOG_DIR", "DATA_DIR", "PID_DIR"};

    for(size_t i=0; i<sizeof(keys)/sizeof(keys[0]); i++) {
        if(dotEnv.find(keys[i]) != dotEnv.end() || getenv(keys[i]) != NULL) {     // set in .env or as real env var? keep that
            continue;
        }

        dotEnv[keys[i]] = defaultDir(keys[i]);
        logHdd(LOG_DEBUG, "Utils::dotEnvAddDefaultDirs - %s not set, using default %s", keys[i], dotEnv[keys[i]].c_str());
    }
}

std::string Utils::dotEnvValue(std::string key, const char* defValue, bool logOnError)
{
    /* get value from dotEnv map for specified key
       Windows paths (C:\...) - e.g. ${LOCALAPPDATA}/cosmo2ex in the .env for Windows - are converted to POSIX paths for Cygwin,
       the .env is shared with the webserver, which is a native Windows program */

    try {
        std::string& value = dotEnv.at(key);             // get value and return it (if found in map)
        return pathFromUi(value);
    }
    catch (const std::out_of_range&) {
        if(logOnError) {
            logHdd(LOG_DEBUG, "Utils::dotEnvValue - no value for key '%s' !", key.c_str());
        }
    }

    // if got here, the value wasn't found in map, but it still could be a real env var, so try getting it
    char* envVar = getenv(key.c_str());

    if(envVar) {    // some real env var was found with this name?
        std::string retValueFromEnv = envVar;   // not static - this is called from multiple threads

        if(logOnError) {
            logHdd(LOG_DEBUG, "Utils::dotEnvValue - ...but found env var '%s' with value '%s'", key.c_str(), retValueFromEnv.c_str());
        }
        return pathFromUi(retValueFromEnv);     // e.g. BIN_DIR set by the ce_run launcher on Windows
    }

    // if value not found and default value was provided, use it; otherwise return empty string
    std::string defValueStr = defValue ? std::string(defValue) : std::string("");
    return defValueStr;
}

std::string Utils::dotEnvExpand(const std::string& value, int depth)
{
    /* return value with all ${VAR}, ${VAR:-default} and ${VAR-default} replaced, like docker compose does:
        - VAR is taken from .env (expanded recursively, so order of lines in .env doesn't matter), then from real env vars
        - ${VAR:-default} uses default when VAR is not set or is empty, ${VAR-default} only when VAR is not set
        - default can contain other vars, e.g. ${A:-${B}}
        - unclosed ${ is left as it is
    */

    if(depth > 10) {        // too deep? probably vars referencing each other in a loop, don't expand anymore
        logHdd(LOG_ERROR, "Utils::dotEnvExpand - too deep recursion, are some .env vars referencing each other? value: '%s'", value.c_str());
        return value;
    }

    std::string result;
    size_t pos = 0;

    while(pos < value.length()) {
        size_t varStart = value.find("${", pos);        // var start tag

        if(varStart == std::string::npos) {             // no more vars? copy the rest and quit
            result += value.substr(pos);
            break;
        }

        // find matching '}' - count nested ${ } pairs, so default value can contain vars
        size_t varEnd = std::string::npos;
        int nested = 0;
        for(size_t i = varStart + 2; i < value.length(); i++) {
            if(value.compare(i, 2, "${") == 0) {
                nested++;
                i++;
            } else if(value[i] == '}') {
                if(nested == 0) {
                    varEnd = i;
                    break;
                }
                nested--;
            }
        }

        if(varEnd == std::string::npos) {               // unclosed ${ ? copy the rest as it is and quit
            result += value.substr(pos);
            break;
        }

        result += value.substr(pos, varStart - pos);    // copy text before the var

        // split var name from default value - var name ends at first '-' (var names don't contain '-')
        std::string varName = value.substr(varStart + 2, varEnd - varStart - 2);
        std::string defValue;
        bool hasDefault = false, defaultIfEmpty = false;

        size_t dashPos = varName.find('-');
        if(dashPos != std::string::npos) {
            hasDefault = true;
            defValue = varName.substr(dashPos + 1);

            if(dashPos > 0 && varName[dashPos - 1] == ':') {    // ':-' means use default also when var is empty
                defaultIfEmpty = true;
                dashPos--;
            }

            varName = varName.substr(0, dashPos);
        }

        // find var value - first in .env, then in real env vars
        bool isSet = false;
        std::string varValue;

        std::map<std::string, std::string>::iterator it = dotEnv.find(varName);
        if(it != dotEnv.end()) {                        // found in .env? expand it too
            isSet = true;
            varValue = dotEnvExpand(it->second, depth + 1);
        } else {
            char* envVar = getenv(varName.c_str());

            if(envVar) {                                // found in real env vars?
                isSet = true;
                varValue = envVar;
            }
        }

        if(hasDefault && (!isSet || (defaultIfEmpty && varValue.empty()))) {    // should use default value?
            varValue = dotEnvExpand(defValue, depth + 1);
        }

        result += varValue;                             // var replaced with its value
        pos = varEnd + 1;                               // continue after the var
    }

    return result;
}

int Utils::dotEnvSubstituteVars(void)
{
    /* go through the current dotEnv values and replace vars with values */

    logHdd(LOG_DEBUG, "Utils::dotEnvSubstituteVars starting");

    // expand all values from original (not yet expanded) values first, store them after that,
    // so the result doesn't depend on the order in which the values are processed
    std::map<std::string, std::string> expanded;
    int found = 0;

    for(std::map<std::string, std::string>::iterator it = dotEnv.begin(); it != dotEnv.end(); ++it) {
        expanded[it->first] = dotEnvExpand(it->second, 0);

        if(expanded[it->first] != it->second) {     // something was replaced in this value?
            found++;
        }
    }

    dotEnv = expanded;
    return found;
}

static bool isBlankChar(char c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static std::string trimBlank(const std::string& s)
{
    size_t start = 0, end = s.length();

    while(start < end && isBlankChar(s[start])) {       // skip leading blank chars
        start++;
    }

    while(end > start && isBlankChar(s[end - 1])) {     // skip trailing blank chars
        end--;
    }

    return s.substr(start, end - start);
}

bool Utils::parseDotEnvLine(const char* line, std::string& key, std::string& value)
{
    /* parse one .env line in the same way as docker compose does for the simple cases:
        - empty lines and lines starting with '#' are skipped
        - key is everything before the 1st '=', value is everything after it (so value can contain '=')
        - value in "double" or 'single' quotes is taken as it is (can contain spaces and '#'), rest of line after closing quote is ignored
        - unquoted value ends at ' #' (inline comment), leading and trailing blank chars are trimmed (spaces inside are kept)
    */

    std::string lineStr = trimBlank(line);

    if(lineStr.empty() || lineStr[0] == '#') {      // empty line or comment? nothing here
        return false;
    }

    size_t eqlPos = lineStr.find('=');              // the 1st '=' splits key and value

    if(eqlPos == std::string::npos) {               // no '=' here? not a key-value line
        return false;
    }

    key = trimBlank(lineStr.substr(0, eqlPos));

    if(key.empty()) {                               // no key? skip
        return false;
    }

    value = trimBlank(lineStr.substr(eqlPos + 1));

    if(!value.empty() && (value[0] == '"' || value[0] == '\'')) {     // quoted value?
        size_t closingQuote = value.find(value[0], 1);

        if(closingQuote != std::string::npos) {     // closing quote found? take what is between the quotes
            value = value.substr(1, closingQuote - 1);
            return true;
        }
        // no closing quote - take the value as unquoted
    }

    // unquoted value - inline comment starts with '#' after blank char
    for(size_t i=1; i<value.length(); i++) {
        if(value[i] == '#' && isBlankChar(value[i - 1])) {
            value = trimBlank(value.substr(0, i));
            break;
        }
    }

    return true;
}

bool Utils::loadDotEnvFrom(const char* path)
{
    /* try to load .env file from the specified path */

    FILE *f = fopen(path, "r");        // try to open file

    if(!f) {
        logHdd(LOG_ERROR, "Utils::loadDotEnv - failed to open file %s", path);
        return false;
    }

    char line[1024];

    while(fgets(line, sizeof(line), f) != NULL) {   // go through file line by line
        std::string key, value;

        if(!parseDotEnvLine(line, key, value)) {    // empty line, comment or no '=' there? skip it
            continue;
        }

        //logHdd(LOG_DEBUG, "Utils::loadDotEnv - found %s -> %s", key.c_str(), value.c_str());
        dotEnv[key] = value;        // store to map
    }

    fclose(f);
    logHdd(LOG_INFO, "Utils::loadDotEnv - loaded file %s", path);

    return true;
}

void Utils::intToFileFromEnv(int value, const char* envKeyForFileName)
{
    std::string fileNameFromEnv = Utils::dotEnvValue(envKeyForFileName);    // fetch filename from env by key
    Utils::intToFile(value, fileNameFromEnv.c_str());                       // int to filename
}

void Utils::intToFile(int value, const char* filePath)
{
    char bfr[128];
    int lastIndex = sizeof(bfr) - 1;
    bfr[lastIndex] = 0;                     // zero terminate buffer

    snprintf(bfr, lastIndex, "%d", value);  // integer to string
    Utils::textToFile(bfr, filePath);       // string to file
}

void Utils::textToFileFromEnv(const char* text, const char* envKeyForFileName)
{
    std::string fileNameFromEnv = Utils::dotEnvValue(envKeyForFileName);    // fetch filename from env by key
    Utils::textToFile(text, fileNameFromEnv.c_str());                       // text to filename
}

void Utils::textToFile(const char* text, const char* filePath)
{
    if(!filePath || strlen(filePath) < 1) {     // null path or empty string path? quit
        return;
    }

    FILE *f = fopen(filePath, "w");    // open file

    if(!f) {                            // could not open file? quit
        return;
    }

    fputs(text, f);                     // write text to file
    fclose(f);                          // close file
}

void Utils::textFromFile(char* bfr, uint32_t bfrSize, const char* filePath, bool trimTrailing)
{
    /*
        Read one line from text file specified by path, up to bfrSize of length.
    */

    if(!filePath || strlen(filePath) < 1) {     // null path or empty string path? quit
        return;
    }

    FILE *f = fopen(filePath, "r");    // open file

    if(!f) {                            // could not open file? quit
        return;
    }

    fgets(bfr, bfrSize, f);             // try to read
    fclose(f);                          // close file

    if(trimTrailing) {                  // if should also trim trailing white spaces, do that
        Utils::trimTrail(bfr);
    }
}

void Utils::trimTrail(char *bfr)
{
    /* trim trailing white spaces from the specified buffer */

    int len = strlen(bfr);

    for(int i = (len-1); i >= 0; i--) {
        if(bfr[i] == '\n' || bfr[i] == '\r' || bfr[i] == '\t' || bfr[i] == ' ') {   // blank char found?
            bfr[i] = 0;     // clear
            continue;       // try next char
        }

        break;              // if got here, this char is not blank, wasn't cleared and we should stop
    }
}

void Utils::screenShotVblEnabled(bool enabled)
{
    events.screenShotVblEnabled = enabled;
    Utils::intToFileFromEnv((int) enabled, "SCREENSHOT_VBL_ENABLED_FILE");        // new value to file
}

void Utils::createFloppyTestImage(void)
{
    // open the file and write to it
    FILE *f = fopen(FDD_TEST_IMAGE_PATH_AND_FILENAME.c_str(), "wb");

    if(!f) {
        logHdd(LOG_ERROR, "Failed to create floppy test image!");
        printf("Failed to create floppy test image!\n");
        return;
    }

    // first fill the write buffer with simple counter
    uint8_t writeBfr[512];
    int i;
    for(i=0; i<512; i++) {
        writeBfr[i] = (uint8_t) i;
    }

    // write one sector after another...
    int sector, track, side;
    for(track=0; track<80; track++) {
        for(side=0; side<2; side++) {
            for(sector=1; sector<10; sector++) {
                // customize write data
                writeBfr[0] = track;
                writeBfr[1] = side;
                writeBfr[2] = sector;

                fwrite(writeBfr, 1, 512, f);
            }
        }
    }

    // close file and we're done
    fclose(f);
}

bool Utils::startsWith(const std::string& value, const char* head)
{
    return (value.find(head) == 0);       // value starts with start when it's found at index 0
}

bool Utils::endsWith(const std::string& value, const char* tail)
{
    size_t lenTail = strlen(tail);

    if (lenTail > value.size()) {     // ending longer than value? surely doesn't end with ending
        return false;
    }

    // compare this value at its end with the ending
    return (value.compare(value.size() - lenTail, lenTail, tail) == 0);
}

void Utils::closeFdIfOpen(int& fd)
{
    if(fd != FD_EMPTY) {    // got the fd?
        close(fd);          // close it
        fd = FD_EMPTY;      // set it to invalid value
    }
}

// threads successfully created by handlePthreadCreate() and not joined yet - pthread_kill_join() must not touch other pthread_t values,
// because when pthread_create() fails, the pthread_t is not set and using it is undefined behavior
static std::vector<pthread_t> createdThreads;
static pthread_mutex_t createdThreadsMutex = PTHREAD_MUTEX_INITIALIZER;

void handlePthreadCreate(const char* threadName, pthread_t* pThreadInfo, ThreadFunc threadCode)
{
    int res = pthread_create(pThreadInfo, NULL, threadCode, NULL);

    if(res != 0) {      // app can't work without any of its threads - terminate whole app with error
        logHdd(LOG_ERROR, "Failed to create %s thread (%s), terminating", threadName, strerror(res));
        printf("Failed to create %s thread (%s), terminating\n", threadName, strerror(res));
        fatalErrorOccurred = 1;
        sigintReceived = 1;
    } else {
        logHdd(LOG_DEBUG, "%s thread created", threadName);
#if !defined(__APPLE__)      // macOS can only name the calling thread - there the thread stays unnamed
        pthread_setname_np(*pThreadInfo, threadName);
#endif

        pthread_mutex_lock(&createdThreadsMutex);
        createdThreads.push_back(*pThreadInfo);         // remember it, so pthread_kill_join() knows it can kill and join it
        pthread_mutex_unlock(&createdThreadsMutex);
    }
}

void pthread_kill_join(const char* threadName, pthread_t& threadInfo)
{
    // was this thread really created (and not joined yet)? if so, remove it from the list, as it's joined below
    bool found = false;

    pthread_mutex_lock(&createdThreadsMutex);
    for(size_t i=0; i<createdThreads.size(); i++) {
        if(pthread_equal(createdThreads[i], threadInfo)) {
            createdThreads.erase(createdThreads.begin() + i);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&createdThreadsMutex);

    if(!found) {        // thread not created (or already joined)? nothing to stop
        printf("Not stopping %s thread, it was not started\n", threadName);
        return;
    }

    printf("Stoping %s thread\n", threadName);
    pthread_kill(threadInfo, SIGINT);           // stop the select()
    pthread_join(threadInfo, NULL);             // wait until thread finishes
}

// Is writeFileAtomically() of this file running now (in this or another process)? Its temp file exists then.
bool Utils::atomicWriteInProgress(const std::string& path)
{
    std::string dir, file;
    splitFilenameFromPath(path, dir, file);
    std::string tmpPrefix = file + ".tmp-";             // see the temp file name in writeFileAtomically()

    DIR *d = opendir(dir.empty() ? "." : dir.c_str());
    if(!d) {
        return false;
    }

    bool found = false;
    struct dirent *de;
    while((de = readdir(d)) != NULL) {
        if(strncmp(de->d_name, tmpPrefix.c_str(), tmpPrefix.length()) == 0) {
            found = true;
            break;
        }
    }

    closedir(d);
    return found;
}

bool Utils::writeFileAtomically(const char* path, const uint8_t* data, size_t size)
{
    // if the path is a symlink, replace the file it points to, not the symlink itself
    char realPath[PATH_MAX];
    if(realpath(path, realPath) == NULL) {          // file doesn't exist (yet)? just use the path as is
        snprintf(realPath, sizeof(realPath), "%s", path);
    }

    // unique temp file name in the same dir (rename() is atomic only within one filesystem) - unique per process and call,
    // so multiple threads / processes can write at the same time
    static std::atomic<unsigned> tmpCounter(0);
    char tmpPath[PATH_MAX + 64];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp-%d-%u", realPath, (int) getpid(), tmpCounter++);

    // mode 0666: the kernel applies the umask, like fopen() would (changing umask to read it isn't thread safe)
    int fd = open(tmpPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if(fd < 0) {
        return false;
    }

    struct stat st;
    if(stat(realPath, &st) == 0) {                  // existing file? keep its permissions
        fchmod(fd, st.st_mode & 07777);
    }

    bool good = true;
    size_t done = 0;
    while(done < size) {                            // write() can write less than asked
        ssize_t res = write(fd, data + done, size - done);
        if(res < 0) {
            if(errno == EINTR) {
                continue;
            }
            good = false;
            break;
        }
        done += res;
    }

    if(good && fsync(fd) != 0) {                    // make sure the data is on disk before the rename makes it 'the' file
        good = false;
    }

    if(close(fd) != 0) {
        good = false;
    }

    if(!good || rename(tmpPath, realPath) != 0) {   // failed? original file stays untouched, remove temp file
        int err = errno;
        unlink(tmpPath);
        errno = err;                                // caller logs the real reason
        return false;
    }

    // make the rename itself durable - fsync the directory
    char dirBuf[PATH_MAX];
    snprintf(dirBuf, sizeof(dirBuf), "%s", realPath);
    int dirFd = open(dirname(dirBuf), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dirFd >= 0) {
        fsync(dirFd);
        close(dirFd);
    }

    return true;
}
