#ifndef _BROWSERLINK_H_
#define _BROWSERLINK_H_

#include <stdint.h>
#include <sys/types.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Link of the browser module to ce_browser - the separate program which runs headless Chromium and converts the pages
// to Atari screens (../../../ce_browser). It listens on a free port on 127.0.0.1, starts ce_browser (BROWSER_BIN in
// .env) pointed at that port, and keeps the newest frame and status ce_browser sent - so ACSI commands are answered
// right away from what is here, never waiting for Chromium. Messages: browserlink_protocol.h.

struct BrowserFrame
{
    uint32_t seq;
    uint8_t  res;                       // SCREENCAST_RES_*
    uint8_t  zoom;                      // in quarters
    uint16_t width;                     // pixels, multiple of 16
    uint16_t height;
    uint8_t  planes;
    std::vector<uint16_t> palette;      // as for the palette registers
    std::vector<uint8_t>  data;         // height rows of rowBytes(), interleaved bitplanes

    uint32_t rowBytes(void) const { return (uint32_t) (width / 16) * planes * 2; }
};

struct BrowserMode
{
    uint8_t  res    = 0;
    uint8_t  zoom   = 4;
    uint16_t width  = 0;
    uint16_t height = 0;
    uint8_t  planes = 0;
    bool     valid  = false;
};

struct BrowserStatus
{
    bool        connected = false;
    uint8_t     flags     = 0;          // BRW_STF_* from ce_browser
    uint8_t     progress  = 0;
    uint16_t    caretX    = 0xffff;
    uint16_t    caretY    = 0xffff;
    uint32_t    latestSeq = 0;
    std::string url;
    std::string title;
    std::string message;
};

class BrowserLink
{
public:
    BrowserLink();
    ~BrowserLink();

    bool start(void);                   // listen + thread if not yet, and start ce_browser if it's not there; false if can't listen
    void stop(void);                    // stop ce_browser (if we started it), the thread, close sockets
    bool isConnected(void) const { return connected; }
    uint16_t port(void) const { return listenPort; }

    // to ce_browser - false when it's not connected (the mode and URL are kept and sent when it connects)
    bool setMode(const BrowserMode& mode);
    bool navigate(const std::string& url);
    bool navAction(uint8_t action);
    bool mouse(uint8_t event, uint16_t x, uint16_t y, uint8_t buttons);
    bool key(uint8_t scancode, uint8_t ascii, uint8_t shift);
    bool scroll(int8_t lines, int8_t pages, int8_t columns, int8_t hpages);

    std::shared_ptr<const BrowserFrame> latestFrame(void);     // newest frame of the current mode, NULL if none yet
    BrowserStatus status(void);

    void noteActivity(void);            // the ST used the browser - ce_browser started by us is stopped after BROWSER_IDLE_MIN without it

    // ce_browser binary (BROWSER_BIN, else BIN_DIR/ce_browser), "" when there's none; CHROME_PATH for its -chrome
    // ("" = its default) - also used by HostTools for 'ce_browser check'
    static std::string browserBin(void);
    static std::string chromePath(void);

private:
    int  fdListen;
    int  fdClient;                      // connection from ce_browser, -1 if none
    std::atomic<uint16_t> listenPort;
    std::atomic<bool> connected;
    std::atomic<bool> threadRun;
    std::thread thread;

    pid_t    childPid;                  // ce_browser started by us, 0 if none
    uint32_t lastSpawnMs;
    bool     spawnedOnce;
    bool     binMissingLogged;
    std::atomic<uint32_t> lastActivityMs;
    std::atomic<bool> wantBrowser;      // CE_BROWS.PRG runs - keep ce_browser running (restart it when it exits)

    std::mutex sendMutex;               // one writer at a time on fdClient
    std::mutex dataMutex;               // everything below
    BrowserMode mode;
    std::string lastUrl;                // sent again when ce_browser (re)connects
    std::shared_ptr<const BrowserFrame> latest;
    BrowserStatus st;
    std::vector<uint8_t> rx;            // received, not yet parsed bytes
    std::string linkMsg;                // status message while ce_browser isn't connected: starting, why it can't run, ...

    void threadCode(void);
    bool openListener(void);
    void acceptClient(void);
    void closeClient(const char *why);
    bool readClient(void);              // false when the connection is gone / broken
    void handleMessage(uint8_t type, const uint8_t *payload, uint32_t len);
    void handleFrame(const uint8_t *p, uint32_t len);
    void handleStatus(const uint8_t *p, uint32_t len);

    bool sendMessage(uint8_t type, const std::vector<uint8_t>& payload);
    static std::vector<uint8_t> modePayload(const BrowserMode& m);

    void spawnBrowser(void);            // start ce_browser if configured, not running and not started too recently (thread only)
    void setLinkMsg(const std::string& msg);
    void checkChild(void);              // reap ce_browser when it exited, stop it when idle
    void stopChild(void);
};

#endif
