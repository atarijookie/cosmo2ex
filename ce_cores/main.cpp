// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <queue>
#include <pty.h>
#include <sys/file.h>
#include <errno.h>
#include <limits.h>

#include "misc/settings.h"
#include "misc/global.h"
#include "hdd/corehdd.h"
#include "fdd/corefdd.h"
#include "misc/debug.h"
#include "misc/version.h"
#include "misc/cmdsockthread.h"
#include "chipinterface/chipinterface.h"
#include "../libdospath/libdospath.h"
#include "discovery/discovery.h"
#include "ikbd/ikbd.h"

volatile sig_atomic_t sigintReceived = 0;
volatile sig_atomic_t fatalErrorOccurred = 0;
void sigint_handler(int sig);

void parseCmdLineArguments(int argc, char *argv[]);
void printfPossibleCmdLineArgs(void);

InterProcessEvents  events;
ExternalServices    externalServices;
bool justShowHelp = false;
int logLevel = LOG_ERROR;
int cmdLineLogLevel = -1;       // log level from command line (llX), -1 = not specified; wins over LOG_LEVEL from .env

pthread_t discoveryThreadInfo;

pthread_t fddThreadInfo;

pthread_t cmdSockThreadInfo;
pthread_t ikbdThreadInfo;

void writePidFile(void);
int runCoreHdd(void);

bool noCapture = false;

int main(int argc, char *argv[])
{

    justShowHelp = false;
    Debug::setLogLevel(LOG_ERROR);      // init current log level to LOG_ERROR

    logHdd(LOG_INFO, "\n\n"); logHdd(LOG_INFO, "---------------------------------------------------");

    parseCmdLineArguments(argc, argv);                          // then parse cmd line arguments and set global variables
    Debug::printfLogLevelString();

    Utils::loadDotEnv();                                       // load dotEnv before setting default log file
    for(int i=LOGFILE_DISCOVERY; i<=LOGFILE_IKBD; i++) {
        Debug::getCoreLogFileName(true, i);    // call this with force=true to re-create the log file name
    }
    if(cmdLineLogLevel < 0) {           // log level not given on command line? take it from .env value of LOG_LEVEL
        Debug::logLevelFromDotEnv();
    }                                   // otherwise the command line one (already set) wins

    ldp_setParam(1, (uint64_t) logLevel);                         // libDOSpath - set log level to file
    std::string logDir = Utils::dotEnvValue("LOG_DIR", LOG_DIR_DEFAULT);  // path to logs dir
    Utils::mergeHostPaths(logDir, "libdospath.log");                    // full path = logs dir + filename
    ldp_setParam(2, (uint64_t) logDir.c_str());                         // libDOSpath - set log file path
    logHdd(LOG_ERROR, "setting libdospath log file to: %s and log level to: %d", logDir.c_str(), logLevel);

    Utils::screenShotVblEnabled(false);                         // screenshot vbl not enabled by default
    preloadGlobalsFromDotEnv();

    //------------------------------------
    // if should only show help and quit
    if(justShowHelp) {
        printfPossibleCmdLineArgs();
        return 0;
    }

    //------------------------------------
    // terminate if some file loaded on start or some dir from .env is missing
    if(!checkStartupFilesAndDirs()) {
        logHdd(LOG_ERROR, "Required files or directories are missing, terminating!");
        printf("\nRequired files or directories are missing, terminating!\n\n\n");
        return 1;
    }

    //------------------------------------
    // before touching USB devices, disks, ports etc. make sure that no other instance is running
    // the command socket is the instance lock, so it's opened here, before any thread starts
    int lockErr = 0;
    if(!cmdSockOpen(lockErr)) {
        if(lockErr == EADDRINUSE) {     // port taken = other instance holds it (or some other program uses CMD_UDP_PORT)
            logHdd(LOG_ERROR, "Other instance of CosmosEx is running (CMD_UDP_PORT is in use), terminate it before starting a new one!");
            printf("\nOther instance of CosmosEx is running (CMD_UDP_PORT is in use), terminate it before starting a new one!\n\n\n");
        } else {                        // without command socket nobody can control the app
            logHdd(LOG_ERROR, "Could not create command socket, terminating");
            printf("\nCould not create command socket, terminating\n\n\n");
        }
        return 1;                       // failure - so a supervisor (systemd, docker) sees that this start didn't work
    }
    writePidFile();

    logHdd(LOG_INFO, "logLevel: %d", logLevel);

    //------------------------------------
    // register signal handlers - SIGINT (Ctrl+C, also used by pthread_kill_join() to wake up threads),
    // SIGTERM (kill, systemd stop) and SIGHUP all do a clean shutdown (save floppy images, remove pid file, ...)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    // no SA_RESTART - blocking calls should return with EINTR, so the loops see sigintReceived quickly

    int signalsToHandle[] = {SIGINT, SIGTERM, SIGHUP};
    for(int sig : signalsToHandle) {
        if(sigaction(sig, &sa, NULL) != 0) {
            printf("Cannot register handler for signal %d!\n", sig);
        }
    }

    // set TZ once here, before any other thread runs - changing the environment while other threads read it
    // (getenv(), localtime_r()) is a data race
    Utils::setTimezoneVariable_inThisContext();

    handlePthreadCreate("discovery service", &discoveryThreadInfo, discoveryMain);
    handlePthreadCreate("command socket", &cmdSockThreadInfo, cmdSockThreadCode);
    handlePthreadCreate("floppy core", &fddThreadInfo, fddThreadCode);
    handlePthreadCreate("ikbd core", &ikbdThreadInfo, ikbdThreadCode);

    int ret = runCoreHdd();

    logHdd(LOG_INFO, "Terminating - stopping threads.");      // logged here, not in signal handler

    pthread_kill_join("ikbd core", ikbdThreadInfo);
    pthread_kill_join("floppy core", fddThreadInfo);
    pthread_kill_join("command socket", cmdSockThreadInfo);
    pthread_kill_join("discovery service", discoveryThreadInfo);

    if(fatalErrorOccurred) {     // terminated because of an error? report failure, so a supervisor (e.g. systemd) can see it
        ret = 1;
    }

    Debug::closeLogFiles();

    return ret;
}

// return path and filename to pid file for this core's instance, include port to distinguish between instances
std::string pidFileName(void)
{
    std::string pidDir = Utils::dotEnvValue("PID_DIR", PID_DIR_DEFAULT);
    std::string pidFilePath = pidDir + std::string("/ce_cores.pid");
    return pidFilePath;
}

int runCoreHdd(void)
{
    CoreHdd *coreHdd;

    Debug::printfLogLevelString();

    char appVersion[16];
    Version::getAppVersion(appVersion);
    logHdd(LOG_INFO, "CosmosEx cores starting");
    printf("\nCosmosEx cores starting\n");


    //-------------
    coreHdd = new CoreHdd();

    printf("Entering main loop...\n");
    coreHdd->run();                // run the main thread
    printf("\n\nExit from main loop\n");

    delete coreHdd;

    // remove PID file on termination
    std::string pidFilePath = pidFileName();
    unlink(pidFilePath.c_str());

    logHdd(LOG_INFO, "CosmosEx terminated.");
    printf("Terminated\n");
    return 0;
}

void parseCmdLineArguments(int argc, char *argv[])
{
    int i;

    for(i=1; i<argc; i++) {                                         // go through all params (when i=0, it's app name, not param)
        int len = strlen(argv[i]);
        if(len < 1) {                                               // argument too short? skip it
            continue;
        }

        bool isKnownTag = false;

        // it's a LOG LEVEL change command (ll)
        if(strncmp(argv[i], "ll", 2) == 0) {
            isKnownTag = true;                                      // this is a known tag
            int ll;

            ll = (int) argv[i][2];

            if(ll >= 48 && ll <= 57) {                              // if it's a number between 0 and 9
                ll = ll - 48;
                Debug::setLogLevel(ll);                             // store log level
                cmdLineLogLevel = ll;                               // remember it - applied again after .env is loaded, so it wins over LOG_LEVEL
            }

            continue;
        }

        // don't capture USB mouse and keyboard
        if(strcmp(argv[i], "nocap") == 0) {
            isKnownTag = true;
            noCapture = true;
        }

        if(strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "/?") == 0 || strcmp(argv[i], "?") == 0) {
            isKnownTag = true;                             // this is a known tag
            justShowHelp  = true;
            continue;
        }

        if(!isKnownTag) {                                           // if tag unknown, show warning
            printf(">>> UNKNOWN APP ARGUMENT: '%s' <<<\n", argv[i]);
        }
    }
}

void printfPossibleCmdLineArgs(void)
{
    printf("\nPossible command line args:\n");
    printf("llx      - set log level to x: 0 off, 1 errors (default), 2 + warnings, 3 + info, 4 + debug (overrides LOG_LEVEL from .env)\n");
    printf("nocap    - don't capture (grab) USB keyboard and mouse\n");
}

void sigint_handler(int sig)
{
    // only set the flag - nothing else is safe in a signal handler (e.g. logging takes a mutex which the interrupted
    // thread might hold -> deadlock); the loops see the flag and terminate, main() logs the shutdown
    sigintReceived = 1;
}

// pid file is just information for humans / scripts (ce_stop.sh), not the lock - see cmdSockOpen()
void writePidFile(void)
{
    std::string pidFilePath = pidFileName();
    Utils::intToFile(getpid(), pidFilePath.c_str());
    logHdd(LOG_DEBUG, "writePidFile - pid %d written to %s", (int) getpid(), pidFilePath.c_str());
}

