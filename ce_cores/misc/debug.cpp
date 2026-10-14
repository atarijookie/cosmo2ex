// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include <string>
#include <map>

#include "misc/global.h"
#include "misc/debug.h"
#include "misc/utils.h"
#include "../libdospath/libdospath.h"
#include "../hdd/native/scsi.h"

uint32_t prevLogOut[4];         // time of previous log line, for each log file - for 'diff ms' column
pthread_mutex_t logFileMutex = PTHREAD_MUTEX_INITIALIZER;   // log files are written from multiple threads - one writer (and log rotation) at a time
uint8_t  g_outToConsole;

DebugVars dbgVars;

std::string coreLogFileName[4];

// log files are kept open (instead of open + close on each line), all of these are accessed with logFileMutex locked
static FILE*        logFile[4];             // open log file or NULL
static std::string  logFileOpenedPath[4];   // path with which logFile was opened - reopen when log file name changes
static long         logFileSize[4];         // current size of log file in bytes - for rotation without stat()
static uint32_t     logFileCheckedMs[4];    // when the log file was last checked for being deleted / moved from outside

static void logFileClose(int whichLog);
static int  logPrintf(int whichLog, FILE* f, const char* format, ...);
static int  logVPrintf(int whichLog, FILE* f, const char* format, va_list args);

const char* Debug::getCoreLogFileName(bool forceCreate, int whichLog)
{
    if(whichLog < LOGFILE_DISCOVERY || whichLog > LOGFILE_IKBD) {     // invalid log file? use HDD log, don't go out of array
        whichLog = LOGFILE_HDD;
    }

    if(coreLogFileName[whichLog].length() > 0 && !forceCreate) {
        return coreLogFileName[whichLog].c_str();
    }

    std::string logDir = Utils::dotEnvValue("LOG_DIR", LOG_DIR_DEFAULT, false);

    switch(whichLog) {
        case LOGFILE_DISCOVERY: coreLogFileName[whichLog] = logDir + std::string("/" DISCOVERY_LOG_FILENAME); break;
        case LOGFILE_HDD: coreLogFileName[whichLog] = logDir + std::string("/" CORE_HDD_LOG_FILENAME); break;
        case LOGFILE_FDD: coreLogFileName[whichLog] = logDir + std::string("/" CORE_FDD_LOG_FILENAME); break;
        case LOGFILE_IKBD: coreLogFileName[whichLog] = logDir + std::string("/" CORE_IKBD_LOG_FILENAME); break;
    }

    return coreLogFileName[whichLog].c_str();
}

void Debug::setOutputToConsole(void)
{
    g_outToConsole = 1;
}

const char* Debug::logLevelString(int ll)
{
    switch(ll) {
        case LOG_OFF:       return "OFF  ";
        case LOG_ERROR:     return "ERROR";
        case LOG_WARNING:   return "WARN ";
        case LOG_INFO:      return "INFO ";
        case LOG_DEBUG:     return "DEBUG";
        default:            return "???  ";
    }
}

void Debug::printfLogLevelString(void)
{
    printf("\nLog level: ");
    printf("%s", logLevelString(logLevel));
    printf("\n\n");
}

void logDiscovery(int aLogLevel, const char *format, ...)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    va_list args;
    va_start(args, format);
    Debug::outV(LOGFILE_DISCOVERY, aLogLevel, format, args);
    va_end(args);
}

void logHdd(int aLogLevel, const char *format, ...)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    va_list args;
    va_start(args, format);
    Debug::outV(LOGFILE_HDD, aLogLevel, format, args);
    va_end(args);
}

void logFdd(int aLogLevel, const char *format, ...)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    va_list args;
    va_start(args, format);
    Debug::outV(LOGFILE_FDD, aLogLevel, format, args);
    va_end(args);
}

void logIkbd(int aLogLevel, const char *format, ...)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    va_list args;
    va_start(args, format);
    Debug::outV(LOGFILE_IKBD, aLogLevel, format, args);
    va_end(args);
}

void Debug::out(int whichLog, int aLogLevel, const char *format, ...)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    va_list args;
    va_start(args, format);
    outV(whichLog, aLogLevel, format, args);
    va_end(args);
}

void Debug::outV(int whichLog, int aLogLevel, const char *format, va_list args)
{
    if(aLogLevel > logLevel) {         // if this log is higher than allowed, don't do this
        return;
    }

    if(whichLog < LOGFILE_DISCOVERY || whichLog > LOGFILE_IKBD) {     // invalid log file? use HDD log, don't go out of arrays
        whichLog = LOGFILE_HDD;
    }

    FILE *f;

    pthread_mutex_lock(&logFileMutex);

    if(g_outToConsole) {                    // should log to console? f is null
        f = NULL;
    } else {                                    // log to file? open the file
        f = logFileOpen(whichLog);
    }

    if(!f) {
        printf("%08u: ", Utils::getCurrentMs());
        vprintf(format, args);
        printf("\n");
        pthread_mutex_unlock(&logFileMutex);
        return;
    }

    uint32_t now = Utils::getCurrentMs();
    uint32_t diff = now - prevLogOut[whichLog];
    prevLogOut[whichLog] = now;

    const char* ll = logLevelString(aLogLevel);

    char humanTime[128];
    struct timeval tv;
    if(gettimeofday(&tv, NULL) < 0) {
        memset(&tv, 0, sizeof(tv)); // failure
    }
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);           // localtime_r() - thread safe version of localtime()
    sprintf(humanTime, "%02d:%02d:%02d.%06ld", tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec);

    if(aLogLevel == LOG_ERROR && dbgVars.isInHandleAcsiCommand) {    // it's an error, and we're debugging ACSI stuff
        logPrintf(whichLog, f, "%s %4d %s\n", humanTime, diff, ll); // diff in ms, date/time in human readable format
        logPrintf(whichLog, f, "     LOG_ERROR occurred\n");
        logPrintf(whichLog, f, "     Time since beginning of ACSI command handling: %d\n", now - dbgVars.thisAcsiCmdTime);
        logPrintf(whichLog, f, "     Time between this and previous ACSI command  : %d\n", dbgVars.thisAcsiCmdTime - dbgVars.prevAcsiCmdTime);
    }

    logPrintf(whichLog, f, "%s %4d %s\t", humanTime, diff, ll); // CLOCK in ms, diff in ms, date/time in human readable format

    logVPrintf(whichLog, f, format, args);
    logPrintf(whichLog, f, "\n");                 // file is line buffered - the line is written to file here

    pthread_mutex_unlock(&logFileMutex);
}

void Debug::outBfr(int whichLog, uint8_t *bfr, int count)
{
    if(logLevel < LOG_DEBUG) {            // if we're not in debug log level, don't do this
        return;
    }

    if(whichLog < LOGFILE_DISCOVERY || whichLog > LOGFILE_IKBD) {     // invalid log file? use HDD log, don't go out of arrays
        whichLog = LOGFILE_HDD;
    }

    pthread_mutex_lock(&logFileMutex);

    FILE* f = g_outToConsole ? stdout : logFileOpen(whichLog);     // log to console or to file

    if(!f) {
        pthread_mutex_unlock(&logFileMutex);
        return;
    }

    logPrintf(whichLog, f, "%08u: outBfr - %d bytes\n", Utils::getCurrentMs(), count);

    int i, j;

    int rows = (count / 16) + (((count % 16) == 0) ? 0 : 1);

    for(i=0; i<rows; i++) {
        int ofs = i * 16;
        logPrintf(whichLog, f, "        ");//some indentation

        for(j=0; j<16; j++) {
            if((ofs + j) < count) {
                logPrintf(whichLog, f, "%02x ", bfr[ofs + j]);
            } else {
                logPrintf(whichLog, f, "   ");
            }
        }

        logPrintf(whichLog, f, "| ");

        for(j=0; j<16; j++) {
            char v = bfr[ofs + j];
            v = (v >= 32 && v <= 126) ? v : '.';

            if((ofs + j) < count) {
                logPrintf(whichLog, f, "%c", v);
            } else {
                logPrintf(whichLog, f, " ");
            }
        }

        logPrintf(whichLog, f, "\n");
    }

    pthread_mutex_unlock(&logFileMutex);
}

void Debug::setLogLevel(int newLogLevel)
{
    if(newLogLevel < LOG_OFF) {         // too low? fix it
        newLogLevel = LOG_OFF;
    }

    if(newLogLevel > LOG_DEBUG) {       // would be higher than highest log level? fix it
        newLogLevel = LOG_DEBUG;
    }

    logHdd(LOG_INFO, "Switching LOG LEVEL from %d to %d", logLevel, newLogLevel);
    logFdd(LOG_INFO, "Switching LOG LEVEL from %d to %d", logLevel, newLogLevel);
    logIkbd(LOG_INFO, "Switching LOG LEVEL from %d to %d", logLevel, newLogLevel);
    logLevel = newLogLevel;                               // new value to struct
    ldp_setParam(1, (uint64_t) logLevel);                 // libDOSpath - set new log level to file
}

// printf to log file (or console) and count written bytes for log rotation; must be called with logFileMutex locked
static int logVPrintf(int whichLog, FILE* f, const char* format, va_list args)
{
    int written = vfprintf(f, format, args);

    if(written > 0 && f == logFile[whichLog]) {     // written to log file (not console)? count it
        logFileSize[whichLog] += written;
    }

    return written;
}

static int logPrintf(int whichLog, FILE* f, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int written = logVPrintf(whichLog, f, format, args);
    va_end(args);
    return written;
}

static void logFileClose(int whichLog)
{
    if(logFile[whichLog]) {
        fclose(logFile[whichLog]);
        logFile[whichLog] = NULL;
    }
}

// true if the open log file is not at the path anymore - deleted or moved from outside (e.g. manual cleanup, logrotate)
static bool logFileGoneFromPath(int whichLog)
{
    struct stat stOpen, stPath;

    if(fstat(fileno(logFile[whichLog]), &stOpen) != 0 || stOpen.st_nlink == 0) {   // can't check open file, or it was deleted?
        return true;
    }

    if(stat(logFileOpenedPath[whichLog].c_str(), &stPath) != 0) {      // nothing at path anymore?
        return true;
    }

    return (stOpen.st_dev != stPath.st_dev || stOpen.st_ino != stPath.st_ino);     // other file at path now?
}

void Debug::closeLogFiles(void)
{
    pthread_mutex_lock(&logFileMutex);

    for(int i=LOGFILE_DISCOVERY; i<=LOGFILE_IKBD; i++) {
        logFileClose(i);
    }

    pthread_mutex_unlock(&logFileMutex);
}

// returns open log file, opens it if needed, rotates it when it's too big; must be called with logFileMutex locked
FILE* Debug::logFileOpen(int whichLog)
{
    const char* path = getCoreLogFileName(false, whichLog);

    if(logFile[whichLog]) {                                     // got open file? check if it's still usable
        if(logFileOpenedPath[whichLog] != path) {               // log file name changed (e.g. LOG_DIR from .env)? open the new one
            logFileClose(whichLog);
        } else if(logFileSize[whichLog] >= LOG_ROTATE_SIZE) {   // file too big? rotate - current to .1, start new one
            logFileClose(whichLog);

            std::string pathOld = std::string(path) + ".1";
            printf("will rotate log file: %s -> %s\n", path, pathOld.c_str());
            unlink(pathOld.c_str());                            // if some previous old file exist, remove it
            rename(path, pathOld.c_str());                      // rename current to old
        } else if(Utils::elapsedMs(logFileCheckedMs[whichLog]) >= LOG_FILE_CHECK_INTERVAL_MS) {    // time to check if it was deleted / moved?
            logFileCheckedMs[whichLog] = Utils::getCurrentMs();

            if(logFileGoneFromPath(whichLog)) {                 // not there anymore? open (create) it again
                logFileClose(whichLog);
            }
        }
    }

    if(!logFile[whichLog]) {                                    // file not open? open it
        FILE* f = fopen(path, "ae");                            // append, close on exec (don't leak fd to child processes)

        if(!f) {
            return NULL;
        }

        setvbuf(f, NULL, _IOLBF, 0);                            // line buffered - each line is written to file at '\n', so tail -f and crash logs work

        fseek(f, 0, SEEK_END);                                  // file might already contain something - continue counting from its size
        logFileSize[whichLog] = ftell(f);

        logFile[whichLog]           = f;
        logFileOpenedPath[whichLog] = path;
        logFileCheckedMs[whichLog]  = Utils::getCurrentMs();
    }

    return logFile[whichLog];
}

void Debug::logLevelFromDotEnv(void)
{
    std::string logLevelStr = Utils::dotEnvValue("LOG_LEVEL", "1");
    int ll;

    ll = (int) logLevelStr.c_str()[0];

    if(ll >= 48 && ll <= 57) {                              // if it's a number between 0 and 9
        ll = ll - 48;
        Debug::setLogLevel(ll);                             // store log level
    }
}
