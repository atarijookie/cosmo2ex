#ifndef _UTILS_H_
#define _UTILS_H_

#include <signal.h>
#include <pthread.h>
#include <string>
#include <vector>

#include <stdint.h>
#include <sys/socket.h>

extern "C" volatile sig_atomic_t sigintReceived;
extern "C" volatile sig_atomic_t fatalErrorOccurred;     // set together with sigintReceived when app terminates because of an error

#ifndef MIN
    #define MIN(x, y)   (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
    #define MAX(x, y)   (((x) > (y)) ? (x) : (y))
#endif

// send() flag: no SIGPIPE when the other side has closed the connection. macOS doesn't have MSG_NOSIGNAL - there
// Utils::socketCloexec() and Utils::acceptCloexec() set SO_NOSIGPIPE on the socket instead
#if defined(MSG_NOSIGNAL)
    #define SEND_NOSIGNAL   MSG_NOSIGNAL
#else
    #define SEND_NOSIGNAL   0
#endif

class Utils {
public:
    static uint32_t getCurrentMs(void);
    static uint32_t elapsedMs(uint32_t startMs);        // ms since startMs (value of getCurrentMs()), wrap safe
    static void  sleepMs(uint32_t ms);

    static void fileDateTimeToHostTime(uint16_t atariDate, uint16_t atariTime, struct tm *ptm);
    static uint16_t fileTimeToAtariTime(struct tm *ptm);
    static uint16_t fileTimeToAtariDate(struct tm *ptm);

    static void mergeHostPaths(std::string &dest, const std::string &tail);                 // this modifies dest
    static std::string mergeHostPaths2(const std::string& head, const std::string& tail);   // this doesn't modify head
    static std::string mergeHostPaths3(const std::string& head, const char* tail);
    static void splitFilenameFromPath(const std::string &pathAndFile, std::string &path, std::string &file);
    static void splitFilenameFromExt(const std::string &filenameAndExt, std::string &filename, std::string &ext);
    static void splitToTwoByDelim(const std::string &input, std::string &beforeDelim, std::string &afterDelim, char delim);
    static void mergeFilenameAndExtension(const std::string& shortFn, const std::string& shortExt, bool extendWithSpaces, std::string& merged);
    static void extendWithSpaces(const char *normalFname, char *extendedFn);

    static void resetHansAndFranz(void);
    static void resetHans(void);
    static void resetFranz(void);

    static bool copyFile(std::string &src, std::string &dst);
    static bool copyFile(FILE *from, std::string &dst);
    static int  mkpath(const char *dir, int mode);

    static void SWAPWORD(uint16_t &w);
    static uint16_t SWAPWORD2(uint16_t w);
    static void swapWordsBuffer(uint8_t *bfr, uint16_t size);
    static void swapWordsBufferWithCopy(uint8_t *bfrDest, uint8_t *bfrSrc, uint16_t size);

    static void getIpAdds(uint8_t *bfrIPs, uint8_t *bfrMasks=NULL);


    static uint16_t  getWord(uint8_t *bfr);
    static uint32_t getDword(uint8_t *bfr);
    static uint32_t get24bits(uint8_t *bfr);

    static void storeWord(uint8_t *bfr, uint16_t val);
    static void storeDword(uint8_t *bfr, uint32_t val);
    static void store24bits(uint8_t *bfr, uint32_t val);

    static void createTimezoneString(char *str);
    static void createTimezoneStringFromOffset(float utcOffset, char *str);    // offset in hours, e.g. 5.5 -> "UTC-05:30"
    static void setTimezoneVariable_inThisContext(void);

    static std::string getDeviceLabel(const std::string & devicePath);
    static std::string decodeHexEscapes(const std::string& in);    // "My\\x20Disk" -> "My Disk", invalid escapes are kept

    static void splitString(const std::string &s, char delim, std::vector<std::string> &elems);

    static const char *getExtension(const char *fileName);
    static void createPathWithOtherExtension(std::string &inPathWithOriginalExt, const char *otherExtension, std::string &outPathWithOtherExtension);
    static bool fileExists(const std::string& path);
    static bool fileExists(const char* path);
    static bool dirExists(std::string& path);
    static bool devExists(std::string& path);

    // socket() / accept() whose fd is not inherited by processes started by system() - atomically where the system can
    static int socketCloexec(int domain, int type, int protocol);
    static int acceptCloexec(int fdListen, struct sockaddr *addr, socklen_t *addrLen);
    static void toUpperCaseString(std::string &st);

    static int bcdToInt(int bcd);

    static std::string dotEnvValue(std::string key, const char* defValue=NULL, bool logOnError=true);
    static void loadDotEnv(void);
    static std::string defaultDir(const std::string& key);  // per-OS default of LOG_DIR / DATA_DIR / PID_DIR, "" for other keys

    // Paths as the user sees them in the web UI <-> paths ce_cores works with. On Windows (Cygwin) the UI has Windows paths
    // (C:\Atari\hdd, \\server\share), ce_cores POSIX ones (/cygdrive/c/Atari/hdd, //server/share). Elsewhere both are the same.
    // Paths which are not of the expected kind (e.g. POSIX path typed in the UI) are returned unchanged.
    // dotEnvValue() uses pathFromUi() too - the .env is shared with the webserver, a native Windows program.
    static bool isWindowsPath(const std::string& path);     // C:\..., C:/..., \\server\...
    static std::string pathFromUi(const std::string& uiPath);
    static std::string pathToUi(const std::string& hostPath);
    static void dotEnvAddDefaultDirs(void);                 // add defaultDir() for those dirs which are neither in .env nor real env vars
    static bool loadDotEnvFrom(const char* path);
    static bool parseDotEnvLine(const char* line, std::string& key, std::string& value);   // false if line has no key-value pair
    static int  dotEnvSubstituteVars(void);

    static void intToFile(int value, const char* filePath);                 // int to text file
    static void intToFileFromEnv(int value, const char* envKeyForFileName); // int to text file specified in .env
    static void textToFile(const char* text, const char* filePath);

    // Write whole file so that a crash / power loss / concurrent reader never sees a half-written or empty file:
    // write to temp file next to it, fsync, then rename over the original. Keeps permissions of existing file,
    // writes through symlink. Returns false (errno set) on failure - original file is then untouched.
    static bool writeFileAtomically(const char* path, const uint8_t* data, size_t size);         // text to text file
    static void textToFileFromEnv(const char* text, const char* envKeyForFileName); // text to text file specified in .env

    static void textFromFile(char* bfr, uint32_t bfrSize, const char* filePath, bool trimTrailing=true); // get one line from text file
    static void trimTrail(char *bfr);

    static bool startsWith(const std::string& value, const char* head);
    static bool endsWith(const std::string& value, const char* tail);

    static void screenShotVblEnabled(bool enabled);
    static void createFloppyTestImage(void);


    static void closeFdIfOpen(int& fd);

private:
    static bool copyFileByHandles(FILE *from, FILE *to);
    static std::string dotEnvExpand(const std::string& value, int depth);   // value with ${VAR} replaced
};

// Timeout measured from creation (or last restart()), safe across wraparound of the 32-bit ms counter
// (getCurrentMs() wraps every ~49.7 days), because it only ever uses elapsed time = now - start,
// which unsigned arithmetic computes correctly even after the wrap.
class Timeout
{
public:
    Timeout(uint32_t timeoutMs) : startMs(Utils::getCurrentMs()), durationMs(timeoutMs) { }

    void restart(void)                      { startMs = Utils::getCurrentMs(); }
    void restart(uint32_t timeoutMs)        { durationMs = timeoutMs; restart(); }

    uint32_t elapsedMs(void) const          { return Utils::elapsedMs(startMs); }
    bool     expired(void)   const          { return elapsedMs() >= durationMs; }
    uint32_t leftMs(void)    const          { uint32_t e = elapsedMs(); return (e < durationMs) ? (durationMs - e) : 0; }

private:
    uint32_t startMs;
    uint32_t durationMs;
};

typedef void* (*ThreadFunc)(void*);         // the only function type pthread_create() may call

void handlePthreadCreate(const char* threadName, pthread_t* pThreadInfo, ThreadFunc threadCode);
void pthread_kill_join(const char* threadName, pthread_t& threadInfo);

#endif
