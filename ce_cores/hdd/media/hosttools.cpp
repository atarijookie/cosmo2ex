// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <unistd.h>
#include <atomic>
#include <thread>
#include <vector>

#include "../../misc/utils.h"
#include "../../misc/childprocess.h"
#include "../../misc/debug.h"
#include "medialink.h"
#include "../browser/browserlink.h"
#include "hosttools.h"

#define CHECK_INTERVAL_MS   30000       // the config drive listed again after this - check again (the user may have
                                        // run ce_setup_tools meanwhile)
#define CHECK_TIMEOUT_MS    15000

static std::atomic<int>      toolState[HostTools::TOOL_COUNT] = { {-1}, {-1}, {-1} };
static std::atomic<bool>     checking(false);
static std::atomic<bool>     forTests(false);
static std::atomic<uint32_t> lastCheckMs(0);
static std::atomic<bool>     checkedOnce(false);

static const char *toolName[HostTools::TOOL_COUNT] = { "ffmpeg (video / audio players)", "Doom", "Chrome (web browser)" };

void HostTools::refresh(void)
{
    if(forTests) {
        return;
    }
    if(checkedOnce && Utils::elapsedMs(lastCheckMs) < CHECK_INTERVAL_MS) {
        return;
    }
    bool expected = false;
    if(!checking.compare_exchange_strong(expected, true)) {     // one is running already
        return;
    }
    lastCheckMs = Utils::getCurrentMs();
    checkedOnce = true;
    std::thread([] {
        runChecks();
        checking = false;
    }).detach();
}

int HostTools::state(Tool t)
{
    return (t >= 0 && t < TOOL_COUNT) ? toolState[t].load() : -1;
}

void HostTools::resetForTests(void)
{
    forTests = false;
    checkedOnce = false;
    for(int i=0; i<TOOL_COUNT; i++) {
        toolState[i] = -1;
    }
}

bool HostTools::checkRunning(void)
{
    return checking;
}

void HostTools::setStateForTests(Tool t, int s)
{
    forTests = true;
    toolState[t] = s;
}

bool HostTools::hideOnConfigDrive(const std::string& name)
{
    std::string n = name;
    Utils::toUpperCaseString(n);

    Tool t;
    if(n == "CE_VIDEO.TTP" || n == "CE_AUDIO.TTP") {
        t = FFMPEG;
    } else if(n == "CE_DOOM.PRG") {
        t = DOOM;
    } else if(n == "CE_BROWS.PRG") {
        t = CHROME;
    } else {
        return false;
    }
    return toolState[t] == 0;                                   // not known yet: shown
}

// 1 when the check said yes, 0 no (or the helper isn't there), -1 it didn't finish - keep what was known
int HostTools::runCheck(const std::string& bin, const std::vector<std::string>& args, const char *userEnvKey, bool allowRoot)
{
    if(bin.empty() || !Utils::fileExists(bin)) {
        return 0;
    }
    std::string err;
    pid_t pid = ChildProcess::spawn(bin, args, userEnvKey, allowRoot, err);
    if(pid == 0) {
        logHdd(LOG_DEBUG, "HostTools - %s: %s", bin.c_str(), err.c_str());
        return 0;
    }
    uint32_t start = Utils::getCurrentMs();
    while(Utils::elapsedMs(start) < CHECK_TIMEOUT_MS) {
        int code;
        if(ChildProcess::exited(pid, &code)) {
            return (code == 0) ? 1 : 0;
        }
        usleep(20000);
    }
    ChildProcess::stop(pid, 500);
    logHdd(LOG_WARNING, "HostTools - %s check didn't finish", bin.c_str());
    return -1;
}

void HostTools::runChecks(void)
{
    std::string media = MediaLink::mediaBin();
    int res[TOOL_COUNT];

    for(int i=0; i<2; i++) {                                    // ffmpeg, doom
        std::vector<std::string> args = { "check", (i == 0) ? "ffmpeg" : "doom" };
        MediaLink::appendOptionArgs(args);
        res[i] = runCheck(media, args, "MEDIA_USER", true);
    }

    std::vector<std::string> args = { "check" };
    std::string chrome = BrowserLink::chromePath();
    if(!chrome.empty()) {
        args.push_back("-chrome");
        args.push_back(chrome);
    }
    res[CHROME] = runCheck(BrowserLink::browserBin(), args, "BROWSER_USER", false);

    if(forTests) {
        return;
    }
    for(int i=0; i<TOOL_COUNT; i++) {
        if(res[i] >= 0 && res[i] != toolState[i]) {
            logHdd(LOG_INFO, "HostTools - %s: %s", toolName[i], res[i] ? "available" : "missing - its program is hidden on the config drive");
            toolState[i] = res[i];
        }
    }
}
