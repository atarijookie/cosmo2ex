#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "browser_commands.h"
#include "browserlink_protocol.h"
#include "browserlink.h"
#include "browserhistory.h"

extern char **environ;

#define SPAWN_RETRY_MS          10000   // don't start ce_browser again sooner than this (it crashes at start, ...)
#define SEND_TIMEOUT_MS         500     // a send to ce_browser which can't go out in this time fails, so the ST isn't held up
#define POLL_MS                 200

BrowserLink::BrowserLink()
{
    fdListen        = -1;
    fdClient        = -1;
    listenPort      = 0;
    connected       = false;
    threadRun       = false;
    childPid        = 0;
    lastSpawnMs     = 0;
    spawnedOnce     = false;
    binMissingLogged = false;
    lastActivityMs  = Utils::getCurrentMs();
    wantBrowser     = false;
}

BrowserLink::~BrowserLink()
{
    stop();
}

//--------------------------------------------------------
// start / stop

bool BrowserLink::start(void)
{
    noteActivity();

    if(fdListen < 0 && !openListener()) {
        return false;
    }

    wantBrowser = true;                                         // the thread starts ce_browser, and restarts it when it exits

    if(!threadRun) {
        threadRun = true;
        thread = std::thread(&BrowserLink::threadCode, this);
    }
    return true;
}

void BrowserLink::setLinkMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    linkMsg = msg;
}

void BrowserLink::stop(void)
{
    if(threadRun) {
        threadRun = false;
        if(thread.joinable()) {
            thread.join();
        }
    }

    stopChild();
    closeClient("stopping");

    if(fdListen >= 0) {
        close(fdListen);
        fdListen = -1;
        listenPort = 0;
    }
}

bool BrowserLink::openListener(void)
{
    int fd = Utils::socketCloexec(AF_INET, SOCK_STREAM, 0);     // CLOEXEC - ce_browser must not inherit it
    if(fd < 0) {
        logHdd(LOG_ERROR, "BrowserLink::openListener - socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);              // only local - ce_browser runs on this host
    addr.sin_port        = 0;                                   // any free port - each device has its own link

    socklen_t len = sizeof(addr);
    if(bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 1) < 0 ||
       getsockname(fd, (struct sockaddr *) &addr, &len) < 0) {
        logHdd(LOG_ERROR, "BrowserLink::openListener - bind / listen failed: %s", strerror(errno));
        close(fd);
        return false;
    }

    fdListen   = fd;
    listenPort = ntohs(addr.sin_port);
    logHdd(LOG_INFO, "BrowserLink - waiting for ce_browser on 127.0.0.1:%d", (int) listenPort);
    return true;
}

//--------------------------------------------------------
// ce_browser process

std::string BrowserLink::browserBin(void)
{
    std::string bin = Utils::dotEnvValue("BROWSER_BIN", "", false);
    if(bin.empty()) {                                           // default: next to ce_cores (as the releases install it)
        std::string binDir = Utils::dotEnvValue("BIN_DIR", "", false);
        for(const char *name : { "/ce_browser", "/ce_browser.exe" }) {
            if(!binDir.empty() && Utils::fileExists(binDir + name)) {
                bin = binDir + name;
                break;
            }
        }
    }
    return bin;
}

std::string BrowserLink::chromePath(void)
{
    // CHROME_PATH in .env (set by ce_setup_tools) - .env values are not in our environment, so it goes as -chrome;
    // in the UI form, ce_browser is a native Windows program there
    std::string chrome = Utils::dotEnvValue("CHROME_PATH", "", false);
    if(!chrome.empty()) {
        chrome = Utils::pathToUi(chrome);
    }
    return chrome;
}

void BrowserLink::spawnBrowser(void)
{
    if(childPid != 0) {                                         // already started, just not connected yet
        return;
    }
    if(spawnedOnce && Utils::elapsedMs(lastSpawnMs) < SPAWN_RETRY_MS) {
        return;
    }

    std::string bin = browserBin();
    if(bin.empty()) {
        if(!binMissingLogged) {
            logHdd(LOG_INFO, "BrowserLink - BROWSER_BIN not set in .env, start it yourself: ce_browser serve -connect 127.0.0.1:%d",
                   (int) listenPort);
            binMissingLogged = true;
        }
        setLinkMsg("ce_browser not configured - BROWSER_BIN in .env");
        return;
    }

    if(!Utils::fileExists(bin)) {                               // say so - exec would only fail with 127
        if(!binMissingLogged) {
            logHdd(LOG_ERROR, "BrowserLink - BROWSER_BIN %s doesn't exist", bin.c_str());
            binMissingLogged = true;
        }
        setLinkMsg("ce_browser not found: " + bin);
        return;
    }
    binMissingLogged = false;

    char portArg[32];
    snprintf(portArg, sizeof(portArg), "127.0.0.1:%d", (int) listenPort);
    std::string chrome = chromePath();
    std::vector<char *> argvVec = { (char *) bin.c_str(), (char *) "serve", (char *) "-connect", portArg };
    if(!chrome.empty()) {
        argvVec.push_back((char *) "-chrome");
        argvVec.push_back((char *) chrome.c_str());
    }
    argvVec.push_back(NULL);
    char **argv = argvVec.data();

    lastSpawnMs = Utils::getCurrentMs();
    spawnedOnce = true;

    // ce_cores as root (sudo): ce_browser runs as a normal user - a web browser must not run as root, and Chrome
    // refuses to (its sandbox needs a normal user). BROWSER_USER in .env, else the user who ran sudo.
    struct passwd *pw = NULL;
    if(geteuid() == 0) {
        std::string user = Utils::dotEnvValue("BROWSER_USER", "", false);
        if(user.empty() && getenv("SUDO_USER")) {
            user = getenv("SUDO_USER");
        }
        if(user.empty() || user == "root") {
            logHdd(LOG_ERROR, "BrowserLink - ce_cores runs as root: set BROWSER_USER in .env to the user ce_browser should run as");
            setLinkMsg("Can't run ce_browser as root - set BROWSER_USER in .env");
            return;
        }
        pw = getpwnam(user.c_str());
        if(!pw) {
            logHdd(LOG_ERROR, "BrowserLink - BROWSER_USER '%s' doesn't exist", user.c_str());
            setLinkMsg("BROWSER_USER " + user + " doesn't exist");
            return;
        }
    }

    pid_t pid;
    if(!pw) {
        int res = posix_spawn(&pid, bin.c_str(), NULL, NULL, argv, environ);
        if(res != 0) {
            logHdd(LOG_ERROR, "BrowserLink - failed to start %s: %s", bin.c_str(), strerror(res));
            setLinkMsg(std::string("Can't start ce_browser: ") + strerror(res));
            return;
        }
    } else {
        // environment of that user: its HOME (ce_browser keeps Chrome and its profile in the user cache dir there)
        std::vector<std::string> env;
        for(char **e = environ; *e; e++) {
            if(strncmp(*e, "HOME=", 5) && strncmp(*e, "USER=", 5) && strncmp(*e, "LOGNAME=", 8) && strncmp(*e, "XDG_", 4)) {
                env.push_back(*e);
            }
        }
        env.push_back(std::string("HOME=") + pw->pw_dir);
        env.push_back(std::string("USER=") + pw->pw_name);
        env.push_back(std::string("LOGNAME=") + pw->pw_name);
        std::vector<char *> envp;
        for(std::string& s : env) {
            envp.push_back((char *) s.c_str());
        }
        envp.push_back(NULL);

        uid_t uid = pw->pw_uid;
        gid_t gid = pw->pw_gid;

        pid = fork();
        if(pid < 0) {
            logHdd(LOG_ERROR, "BrowserLink - fork failed: %s", strerror(errno));
            setLinkMsg(std::string("Can't start ce_browser: ") + strerror(errno));
            return;
        }
        if(pid == 0) {                                          // child - only async-signal-safe calls until exec
            if(setgroups(1, &gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
                _exit(126);
            }
            execve(bin.c_str(), argv, envp.data());
            _exit(127);
        }
    }

    childPid = pid;
    setLinkMsg("Starting ce_browser...");
    logHdd(LOG_INFO, "BrowserLink - started %s serve -connect %s, pid %d%s%s", bin.c_str(), portArg, (int) pid,
           pw ? ", as user " : "", pw ? pw->pw_name : "");
}

void BrowserLink::checkChild(void)
{
    if(childPid == 0) {
        return;
    }

    int wstatus;
    pid_t res = waitpid(childPid, &wstatus, WNOHANG);
    if(res == childPid || (res < 0 && errno == ECHILD)) {       // exited (or reaped elsewhere)
        int code = (res == childPid && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
        logHdd(LOG_INFO, "BrowserLink - ce_browser (pid %d) exited, code %d", (int) childPid, code);
        childPid = 0;
        if(code == 126) {
            setLinkMsg("ce_browser: can't switch to BROWSER_USER");
        } else if(code == 127) {
            setLinkMsg("ce_browser: can't run BROWSER_BIN");
        } else {
            setLinkMsg("ce_browser exited (code " + std::to_string(code) + "), restarting...");
        }
        return;
    }

    int idleMin = atoi(Utils::dotEnvValue("BROWSER_IDLE_MIN", "10", false).c_str());
    if(idleMin > 0 && Utils::elapsedMs(lastActivityMs) >= (uint32_t) idleMin * 60000) {
        logHdd(LOG_INFO, "BrowserLink - browser not used for %d min, stopping ce_browser", idleMin);
        wantBrowser = false;                                    // until CE_BROWS.PRG starts again (IDENTIFY)
        stopChild();
        setLinkMsg("ce_browser stopped after " + std::to_string(idleMin) + " min idle");
    }
}

void BrowserLink::stopChild(void)
{
    if(childPid == 0) {
        return;
    }

    kill(childPid, SIGTERM);
    for(int i=0; i<20; i++) {                                   // give it 2 s to close Chromium, then kill it
        if(waitpid(childPid, NULL, WNOHANG) != 0) {
            childPid = 0;
            return;
        }
        Utils::sleepMs(100);
    }
    kill(childPid, SIGKILL);
    waitpid(childPid, NULL, 0);
    childPid = 0;
}

void BrowserLink::noteActivity(void)
{
    lastActivityMs = Utils::getCurrentMs();
}

//--------------------------------------------------------
// thread: accept ce_browser, read its messages

void BrowserLink::threadCode(void)
{
    while(threadRun) {
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = fdListen; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        int clientFd = fdClient;
        if(clientFd >= 0) {
            pfd[n].fd = clientFd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        }

        int res = poll(pfd, n, POLL_MS);
        if(res > 0) {
            if(pfd[0].revents & POLLIN) {
                acceptClient();
            }
            if(n > 1 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                if(!readClient()) {
                    closeClient("connection closed");
                }
            }
        }

        checkChild();
        if(wantBrowser && !connected) {                         // (re)start it - spawnBrowser() limits how often
            spawnBrowser();
        }
    }
}

void BrowserLink::acceptClient(void)
{
    int fd = Utils::acceptCloexec(fdListen, NULL, NULL);
    if(fd < 0) {
        return;
    }

    if(fdClient >= 0) {                                         // a new ce_browser replaces the old connection
        closeClient("replaced by a new connection");
    }

    struct timeval tv;
    tv.tv_sec  = SEND_TIMEOUT_MS / 1000;
    tv.tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    {
        std::lock_guard<std::mutex> lock(sendMutex);
        fdClient = fd;
    }
    rx.clear();
    connected = true;
    setLinkMsg("");
    logHdd(LOG_INFO, "BrowserLink - ce_browser connected");

    sendMessage(BLINK_HELLO, { (uint8_t) (BLINK_VERSION >> 8), (uint8_t) BLINK_VERSION });

    BrowserMode m;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        m   = mode;
        url = lastUrl;
        st.message.clear();
    }
    if(m.valid) {                                               // ce_browser (re)started - bring it to where the ST is
        sendMessage(BLINK_SET_MODE, modePayload(m));
    }
    if(!url.empty()) {
        sendMessage(BLINK_NAVIGATE, std::vector<uint8_t>(url.begin(), url.end()));
    }
}

void BrowserLink::closeClient(const char *why)
{
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        if(fdClient < 0) {
            return;
        }
        close(fdClient);
        fdClient = -1;
    }
    connected = false;
    rx.clear();

    std::lock_guard<std::mutex> lock(dataMutex);
    latest.reset();                                             // a new ce_browser starts its frames anew
    linkMsg = "ce_browser disconnected";
    logHdd(LOG_INFO, "BrowserLink - ce_browser disconnected: %s", why);
}

bool BrowserLink::readClient(void)
{
    uint8_t buf[64 * 1024];
    ssize_t got = recv(fdClient, buf, sizeof(buf), 0);
    if(got <= 0) {
        return false;
    }
    rx.insert(rx.end(), buf, buf + got);

    size_t pos = 0;
    while(rx.size() - pos >= BLINK_HEADER_SIZE) {
        uint8_t  type = rx[pos];
        uint32_t len  = Utils::getDword(&rx[pos + 1]);
        if(len > BLINK_MAX_PAYLOAD) {
            logHdd(LOG_ERROR, "BrowserLink - message 0x%02x with length %u, closing", type, len);
            return false;
        }
        if(rx.size() - pos - BLINK_HEADER_SIZE < len) {         // not complete yet
            break;
        }
        handleMessage(type, rx.data() + pos + BLINK_HEADER_SIZE, len);
        pos += BLINK_HEADER_SIZE + len;
    }
    rx.erase(rx.begin(), rx.begin() + pos);
    return true;
}

void BrowserLink::handleMessage(uint8_t type, const uint8_t *payload, uint32_t len)
{
    switch(type) {
        case BLINK_HELLO_REPLY:
            if(len >= 2) {
                logHdd(LOG_INFO, "BrowserLink - ce_browser link version %d", (int) Utils::getWord((uint8_t *) payload));
            }
            break;
        case BLINK_FRAME:   handleFrame(payload, len);  break;
        case BLINK_STATUS:  handleStatus(payload, len); break;
        case BLINK_HISTORY_ADD:
            BrowserHistory::add(std::string((const char *) payload, len));
            break;
        default:
            logHdd(LOG_DEBUG, "BrowserLink - unknown message 0x%02x (%u bytes) ignored", type, len);
            break;
    }
}

void BrowserLink::handleFrame(const uint8_t *p, uint32_t len)
{
    if(len < BLINK_FRAME_HEADER_SIZE) {
        logHdd(LOG_WARNING, "BrowserLink - frame too short (%u bytes)", len);
        return;
    }

    auto f = std::make_shared<BrowserFrame>();
    f->seq    = Utils::getDword((uint8_t *) p);
    f->res    = p[4];
    f->zoom   = p[5];
    f->width  = Utils::getWord((uint8_t *) p + 6);
    f->height = Utils::getWord((uint8_t *) p + 8);
    f->planes = p[10];
    uint16_t palCount = Utils::getWord((uint8_t *) p + 12);

    uint32_t palBytes  = (uint32_t) palCount * 2;
    uint32_t dataBytes = f->rowBytes() * f->height;
    if(f->seq == 0 || (f->width % 16) != 0 || palCount > 256 ||
       (f->planes != 1 && f->planes != 2 && f->planes != 4 && f->planes != 8) ||
       len != BLINK_FRAME_HEADER_SIZE + palBytes + dataBytes) {
        logHdd(LOG_WARNING, "BrowserLink - bad frame: seq %u, %dx%d, %d planes, %d colours, %u bytes",
               f->seq, (int) f->width, (int) f->height, (int) f->planes, (int) palCount, len);
        return;
    }

    const uint8_t *pal = p + BLINK_FRAME_HEADER_SIZE;
    f->palette.resize(palCount);
    for(int i=0; i<palCount; i++) {
        f->palette[i] = Utils::getWord((uint8_t *) pal + i * 2);
    }
    f->data.assign(pal + palBytes, pal + palBytes + dataBytes);

    std::lock_guard<std::mutex> lock(dataMutex);
    if(!mode.valid || f->res != mode.res || f->zoom != mode.zoom || f->width != mode.width ||
       f->height != mode.height || f->planes != mode.planes) {     // rendered for the previous mode - would garble the screen
        logHdd(LOG_DEBUG, "BrowserLink - frame %u is not for the current mode, dropped", f->seq);
        return;
    }
    latest = f;
}

// length (W) + bytes, moves p; false if it doesn't fit in what's left
static bool readLenString(const uint8_t *&p, const uint8_t *end, std::string &out)
{
    if(end - p < 2) {
        return false;
    }
    uint16_t len = Utils::getWord((uint8_t *) p);
    p += 2;
    if(end - p < len) {
        return false;
    }
    out.assign((const char *) p, len);
    p += len;
    return true;
}

void BrowserLink::handleStatus(const uint8_t *p, uint32_t len)
{
    const uint8_t *end = p + len;
    if(len < 6) {
        return;
    }

    BrowserStatus s;
    s.flags    = p[0] & ~BRW_STF_CONNECTED;
    s.progress = p[1];
    s.caretX   = Utils::getWord((uint8_t *) p + 2);
    s.caretY   = Utils::getWord((uint8_t *) p + 4);
    const uint8_t *q = p + 6;
    if(!readLenString(q, end, s.url) || !readLenString(q, end, s.title) || !readLenString(q, end, s.message)) {
        logHdd(LOG_WARNING, "BrowserLink - bad status message (%u bytes)", len);
        return;
    }

    std::lock_guard<std::mutex> lock(dataMutex);
    st.flags    = s.flags;
    st.progress = s.progress;
    st.caretX   = s.caretX;
    st.caretY   = s.caretY;
    st.url      = s.url;
    st.title    = s.title;
    st.message  = s.message;
}

//--------------------------------------------------------
// to ce_browser

bool BrowserLink::sendMessage(uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> msg(BLINK_HEADER_SIZE + payload.size());
    msg[0] = type;
    Utils::storeDword(&msg[1], (uint32_t) payload.size());
    if(!payload.empty()) {
        memcpy(&msg[BLINK_HEADER_SIZE], payload.data(), payload.size());
    }

    std::lock_guard<std::mutex> lock(sendMutex);
    if(fdClient < 0) {
        return false;
    }

    size_t sent = 0;
    while(sent < msg.size()) {
        ssize_t res = send(fdClient, msg.data() + sent, msg.size() - sent, SEND_NOSIGNAL);
        if(res <= 0) {
            if(res < 0 && errno == EINTR) {
                continue;
            }
            logHdd(LOG_WARNING, "BrowserLink - send of message 0x%02x failed: %s", type, strerror(errno));
            return false;                                       // the reader thread notices a dead connection
        }
        sent += res;
    }
    return true;
}

std::vector<uint8_t> BrowserLink::modePayload(const BrowserMode& m)
{
    return { m.res, m.zoom, (uint8_t) (m.width >> 8), (uint8_t) m.width, (uint8_t) (m.height >> 8), (uint8_t) m.height, m.planes };
}

bool BrowserLink::setMode(const BrowserMode& m)
{
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        mode = m;
        latest.reset();                                         // frames of the previous mode are of no use
    }
    return sendMessage(BLINK_SET_MODE, modePayload(m));
}

bool BrowserLink::navigate(const std::string& url)
{
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        lastUrl = url;
    }
    return sendMessage(BLINK_NAVIGATE, std::vector<uint8_t>(url.begin(), url.end()));
}

bool BrowserLink::navAction(uint8_t action)
{
    return sendMessage(BLINK_NAV_ACTION, { action });
}

bool BrowserLink::mouse(uint8_t event, uint16_t x, uint16_t y, uint8_t buttons)
{
    return sendMessage(BLINK_MOUSE, { event, (uint8_t) (x >> 8), (uint8_t) x, (uint8_t) (y >> 8), (uint8_t) y, buttons });
}

bool BrowserLink::key(uint8_t scancode, uint8_t ascii, uint8_t shift)
{
    return sendMessage(BLINK_KEY, { scancode, ascii, shift });
}

bool BrowserLink::scroll(int8_t lines, int8_t pages, int8_t columns, int8_t hpages)
{
    return sendMessage(BLINK_SCROLL, { (uint8_t) lines, (uint8_t) pages, (uint8_t) columns, (uint8_t) hpages });
}

//--------------------------------------------------------
// what the ACSI side reads

std::shared_ptr<const BrowserFrame> BrowserLink::latestFrame(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return latest;
}

BrowserStatus BrowserLink::status(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    BrowserStatus s = st;
    s.connected = connected;
    s.latestSeq = latest ? latest->seq : 0;
    if(!s.connected) {                                          // what ce_browser said last is old news - say why it's not here
        s.flags    = 0;
        s.progress = 0;
        s.message  = linkMsg.empty() ? "ce_browser is not running" : linkMsg;
    }
    return s;
}
