#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdint.h>
#include <stdarg.h>
#include <cstdio>

#define DISCOVERY_LOG_FILENAME  "discovery.log"
#define CORE_HDD_LOG_FILENAME   "core_hdd.log"
#define CORE_FDD_LOG_FILENAME   "core_fdd.log"
#define CORE_IKBD_LOG_FILENAME  "core_ikbd.log"

#define LOGFILE_DISCOVERY   0
#define LOGFILE_HDD         1
#define LOGFILE_FDD         2
#define LOGFILE_IKBD        3

#define LOG_ROTATE_SIZE             (1024*1024) // when log file reaches this size, it's renamed to .1 and new one is started
#define LOG_FILE_CHECK_INTERVAL_MS  1000        // how often to check that open log file wasn't deleted / moved from outside

// log levels - higher level shows also all the lower levels, e.g. LOG_WARNING shows errors and warnings
#define LOG_OFF         0
#define LOG_ERROR       1       // errors       - should be always visible, even to users
#define LOG_WARNING     2       // warnings
#define LOG_INFO        3       // info         - info which can be displayed when running at user's place
#define LOG_DEBUG       4       // debug info   - useful only to developers

typedef struct {
    uint8_t    isInHandleAcsiCommand;
    uint32_t   prevAcsiCmdTime;
    uint32_t   thisAcsiCmdTime;
} DebugVars;

void logDiscovery(int logLevel, const char *format, ...);
void logHdd(int logLevel, const char *format, ...);
void logFdd(int logLevel, const char *format, ...);
void logIkbd(int logLevel, const char *format, ...);

class Debug
{
public:
    static void logLevelFromDotEnv(void);

    static void out(int whichLog, int logLevel, const char *format, ...);
    static void outBfr(int whichLog, uint8_t *bfr, int count);
    static void outV(int whichLog, int logLevel, const char *format, va_list args);

    static const char* logLevelString(int ll);
    static void printfLogLevelString(void);

    static void setLogLevel(int newLogLevel);
    static void setOutputToConsole(void);

    static FILE* logFileOpen(int whichLog);     // must be called with log mutex locked
    static void  closeLogFiles(void);
    static const char* getCoreLogFileName(bool forceCreate, int whichLog);
};

#endif

