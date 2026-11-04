#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../../misc/childprocess.h"
#include "media_commands.h"
#include "medialink_protocol.h"
#include "medialink.h"

#define SPAWN_RETRY_MS          10000   // don't start ce_media again sooner than this
#define SEND_TIMEOUT_MS         500
#define POLL_MS                 100
#define DEFAULT_QUEUE_BYTES     (1024 * 1024)   // ~1.5 s of ST video - the ST keeps its own buffer on top of that
#define LIVE_QUEUE_CHUNKS       2               // live streams: only the newest chunks, older are dropped
#define MAX_CHUNK_BYTES         (254 * 512)     // one ACSI transfer

MediaLink::MediaLink()
{
    fdListen         = -1;
    fdClient         = -1;
    listenPort       = 0;
    connected        = false;
    threadRun        = false;
    childPid         = 0;
    lastSpawnMs      = 0;
    spawnedOnce      = false;
    binMissingLogged = false;
    lastActivityMs   = Utils::getCurrentMs();
    wantMedia        = false;

    streamOpen       = false;
    live             = false;
    gen              = 0;
    queueBytes       = 0;
    queueLimit       = DEFAULT_QUEUE_BYTES;
    endGiven         = false;
}

MediaLink::~MediaLink()
{
    stop();
}

//--------------------------------------------------------
// start / stop

bool MediaLink::start(void)
{
    noteActivity();

    if(fdListen < 0 && !openListener()) {
        return false;
    }
    wantMedia = true;

    if(!threadRun) {
        threadRun = true;
        thread = std::thread(&MediaLink::threadCode, this);
    }
    return true;
}

void MediaLink::stop(void)
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
        ::close(fdListen);
        fdListen = -1;
        listenPort = 0;
    }

    std::lock_guard<std::mutex> lock(dataMutex);
    clearStream();
    streamOpen = false;
    openMsg.clear();
}

bool MediaLink::openListener(void)
{
    int fd = Utils::socketCloexec(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        logHdd(LOG_ERROR, "MediaLink::openListener - socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    socklen_t len = sizeof(addr);
    if(bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 1) < 0 ||
       getsockname(fd, (struct sockaddr *) &addr, &len) < 0) {
        logHdd(LOG_ERROR, "MediaLink::openListener - bind / listen failed: %s", strerror(errno));
        ::close(fd);
        return false;
    }

    fdListen   = fd;
    listenPort = ntohs(addr.sin_port);
    logHdd(LOG_INFO, "MediaLink - waiting for ce_media on 127.0.0.1:%d", (int) listenPort);
    return true;
}

void MediaLink::setLinkMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    linkMsg = msg;
}

//--------------------------------------------------------
// ce_media process

void MediaLink::spawnMedia(void)
{
    if(childPid != 0) {
        return;
    }
    if(spawnedOnce && Utils::elapsedMs(lastSpawnMs) < SPAWN_RETRY_MS) {
        return;
    }

    std::string bin = Utils::dotEnvValue("MEDIA_BIN", "", false);
    if(bin.empty()) {                                           // default: next to ce_cores
        std::string binDir = Utils::dotEnvValue("BIN_DIR", "", false);
        for(const char *name : { "/ce_media", "/ce_media.exe" }) {
            if(!binDir.empty() && Utils::fileExists(binDir + name)) {
                bin = binDir + name;
                break;
            }
        }
    }
    if(bin.empty()) {
        if(!binMissingLogged) {
            logHdd(LOG_INFO, "MediaLink - MEDIA_BIN not set in .env, start it yourself: ce_media serve -connect 127.0.0.1:%d",
                   (int) listenPort);
            binMissingLogged = true;
        }
        setLinkMsg("ce_media not configured - MEDIA_BIN in .env");
        return;
    }

    if(!Utils::fileExists(bin)) {                               // say so - exec would only fail with 127
        if(!binMissingLogged) {
            logHdd(LOG_ERROR, "MediaLink - MEDIA_BIN %s doesn't exist", bin.c_str());
            binMissingLogged = true;
        }
        setLinkMsg("ce_media not found: " + bin);
        return;
    }
    binMissingLogged = false;
    childBin = bin;

    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", (int) listenPort);
    std::vector<std::string> args = { "serve", "-connect", addr };

    // paths go back to the UI form: dotEnvValue() makes Cygwin paths of them, but ce_media is a native Windows program there
    struct { const char *key; const char *arg; bool path; } opts[] = {
        { "MEDIA_FFMPEG",       "-ffmpeg",          true },
        { "MEDIA_FFPROBE",      "-ffprobe",         true },
        { "MEDIA_DOCKER_IMAGE", "-docker-image",    false },
        { "MEDIA_YM_TABLE",     "-ym-table",        true },
    };
    for(auto& o : opts) {
        std::string v = Utils::dotEnvValue(o.key, "", false);
        if(!v.empty()) {
            args.push_back(o.arg);
            args.push_back(o.path ? Utils::pathToUi(v) : v);
        }
    }

    lastSpawnMs = Utils::getCurrentMs();
    spawnedOnce = true;

    // as a normal user when ce_cores runs as root - ffmpeg parses files from anywhere; root only when there's no other
    std::string err;
    pid_t pid = ChildProcess::spawn(bin, args, "MEDIA_USER", true, err);
    if(pid == 0) {
        logHdd(LOG_ERROR, "MediaLink - %s", err.c_str());
        setLinkMsg(err);
        return;
    }

    childPid = pid;
    setLinkMsg("Starting ce_media...");
    logHdd(LOG_INFO, "MediaLink - started %s serve -connect %s, pid %d", bin.c_str(), addr, (int) pid);
}

void MediaLink::checkChild(void)
{
    if(childPid != 0) {
        int code;
        if(ChildProcess::exited(childPid, &code)) {
            logHdd(LOG_INFO, "MediaLink - ce_media (pid %d) exited, code %d", (int) childPid, code);
            childPid = 0;
            if(code == 126) {
                setLinkMsg("ce_media: can't switch to MEDIA_USER");
            } else if(code == 127) {                            // exec failed: not executable / readable for MEDIA_USER, wrong CPU
                setLinkMsg("Can't run " + childBin + " (permissions? CPU type?)");
            } else {
                setLinkMsg("ce_media exited (code " + std::to_string(code) + ")");
            }
            return;
        }
    }

    int idleMin = atoi(Utils::dotEnvValue("MEDIA_IDLE_MIN", "10", false).c_str());
    if(wantMedia && idleMin > 0 && Utils::elapsedMs(lastActivityMs) >= (uint32_t) idleMin * 60000) {
        logHdd(LOG_INFO, "MediaLink - not used for %d min, stopping ce_media", idleMin);
        wantMedia = false;                                      // until the ST uses the module again
        stopChild();
        closeClient("idle");
    }
}

void MediaLink::stopChild(void)
{
    if(childPid != 0) {
        ChildProcess::stop(childPid, 6000);             // ce_media removes its ffmpeg containers first
        childPid = 0;
    }
}

void MediaLink::noteActivity(void)
{
    lastActivityMs = Utils::getCurrentMs();
}

//--------------------------------------------------------
// thread

bool MediaLink::queueFull(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return !live && queueBytes >= queueLimit;
}

void MediaLink::threadCode(void)
{
    while(threadRun) {
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = fdListen; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        int clientFd = fdClient;
        if(clientFd >= 0) {                                     // full queue: not read - ce_media waits (HUP still comes)
            pfd[n].fd = clientFd; pfd[n].events = queueFull() ? 0 : POLLIN; pfd[n].revents = 0; n++;
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
        if(wantMedia && !connected) {
            spawnMedia();
        }
    }
}

void MediaLink::acceptClient(void)
{
    int fd = Utils::acceptCloexec(fdListen, NULL, NULL);
    if(fd < 0) {
        return;
    }
    if(fdClient >= 0) {
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
    logHdd(LOG_INFO, "MediaLink - ce_media connected");

    sendMessage(MLINK_HELLO, { (uint8_t) (MLINK_VERSION >> 8), (uint8_t) MLINK_VERSION });

    std::vector<uint8_t> msg;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        linkMsg.clear();
        // opened while ce_media was starting (or it went away while still getting ready) - send it now
        if(streamOpen && (infoBlk.empty() || infoBlk[MED_INFO_STATE] == MED_STATE_STARTING)) {
            msg = openMsg;
        }
    }
    if(!msg.empty()) {
        sendMessage(MLINK_OPEN, msg);
    }
}

void MediaLink::closeClient(const char *why)
{
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        if(fdClient < 0) {
            return;
        }
        ::close(fdClient);
        fdClient = -1;
    }
    connected = false;
    rx.clear();

    std::lock_guard<std::mutex> lock(dataMutex);
    if(streamOpen && !infoBlk.empty() && infoBlk[MED_INFO_STATE] == MED_STATE_READY) {
        // the stream was running - a new ce_media can't continue it, the ST has to open the file again
        infoBlk[MED_INFO_STATE] = MED_STATE_ERROR;
        const char *m = "ce_media stopped";
        memset(&infoBlk[MED_INFO_MSG], 0, MED_INFO_MSG_SIZE);
        memcpy(&infoBlk[MED_INFO_MSG], m, strlen(m));
    }
    linkMsg = "ce_media disconnected";
    logHdd(LOG_INFO, "MediaLink - ce_media disconnected: %s", why);
}

bool MediaLink::readClient(void)
{
    uint8_t buf[64 * 1024];
    ssize_t got = recv(fdClient, buf, sizeof(buf), 0);
    if(got <= 0) {
        return false;
    }
    rx.insert(rx.end(), buf, buf + got);

    size_t pos = 0;
    while(rx.size() - pos >= MLINK_HEADER_SIZE) {
        uint8_t  type = rx[pos];
        uint32_t len  = Utils::getDword(&rx[pos + 1]);
        if(len > MLINK_MAX_PAYLOAD) {
            logHdd(LOG_ERROR, "MediaLink - message 0x%02x with length %u, closing", type, len);
            return false;
        }
        if(rx.size() - pos - MLINK_HEADER_SIZE < len) {
            break;
        }
        handleMessage(type, rx.data() + pos + MLINK_HEADER_SIZE, len);
        pos += MLINK_HEADER_SIZE + len;
    }
    rx.erase(rx.begin(), rx.begin() + pos);
    return true;
}

void MediaLink::handleMessage(uint8_t type, const uint8_t *payload, uint32_t len)
{
    std::lock_guard<std::mutex> lock(dataMutex);

    switch(type) {
        case MLINK_HELLO_REPLY:
            if(len >= 2) {
                logHdd(LOG_INFO, "MediaLink - ce_media link version %d", (int) Utils::getWord((uint8_t *) payload));
            }
            break;

        case MLINK_INFO:
            if(len != MED_INFO_SIZE) {
                logHdd(LOG_WARNING, "MediaLink - INFO of %u bytes ignored", len);
                break;
            }
            if(streamOpen) {
                infoBlk.assign(payload, payload + len);
                if(payload[MED_INFO_STATE] == MED_STATE_ERROR) {
                    logHdd(LOG_WARNING, "MediaLink - ce_media: %s", (const char *) &infoBlk[MED_INFO_MSG]);
                }
            }
            break;

        case MLINK_YM_TABLE:
            if(len == MED_YM_TABLE_SIZE && streamOpen) {
                ym.assign(payload, payload + len);
            }
            break;

        case MLINK_CHUNK: {
            if(len < MED_CHUNK_HEADER_SIZE || len > MAX_CHUNK_BYTES) {
                logHdd(LOG_WARNING, "MediaLink - chunk of %u bytes ignored", len);
                break;
            }
            uint32_t g = Utils::getDword((uint8_t *) payload + MED_CHUNK_GENERATION);
            if(!streamOpen || g != gen) {                       // from before a SEEK / OPEN / CLOSE - read and dropped
                break;
            }
            auto c = std::make_shared<std::vector<uint8_t>>(payload, payload + len);
            queue.push_back(c);
            queueBytes += len;
            while(live && queue.size() > LIVE_QUEUE_CHUNKS) {    // live: the newest count, not every one
                queueBytes -= queue.front()->size();
                queue.pop_front();
            }
            break;
        }

        default:
            logHdd(LOG_DEBUG, "MediaLink - unknown message 0x%02x (%u bytes) ignored", type, len);
            break;
    }
}

//--------------------------------------------------------
// to ce_media

bool MediaLink::sendMessage(uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> msg(MLINK_HEADER_SIZE + payload.size());
    msg[0] = type;
    Utils::storeDword(&msg[1], (uint32_t) payload.size());
    if(!payload.empty()) {
        memcpy(&msg[MLINK_HEADER_SIZE], payload.data(), payload.size());
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
            logHdd(LOG_WARNING, "MediaLink - send of message 0x%02x failed: %s", type, strerror(errno));
            return false;
        }
        sent += res;
    }
    return true;
}

void MediaLink::clearStream(void)
{
    queue.clear();
    queueBytes = 0;
    endGiven   = false;
}

void MediaLink::open(uint32_t g, const uint8_t *openHeader, const std::string& hostPath, bool isLive)
{
    std::vector<uint8_t> p(4 + MED_OPEN_PATH);
    Utils::storeDword(p.data(), g);
    memcpy(p.data() + 4, openHeader, MED_OPEN_PATH);
    p.insert(p.end(), hostPath.begin(), hostPath.end());

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        clearStream();
        streamOpen = true;
        live       = isLive;
        gen        = g;
        openMsg    = p;
        infoBlk.clear();
        ym.clear();
    }
    sendMessage(MLINK_OPEN, p);         // not connected: acceptClient() sends it
}

void MediaLink::seek(uint32_t g, uint32_t posMs)
{
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        clearStream();
        gen = g;
    }
    std::vector<uint8_t> p(8);
    Utils::storeDword(p.data(), g);
    Utils::storeDword(p.data() + 4, posMs);
    sendMessage(MLINK_SEEK, p);
}

bool MediaLink::input(const uint8_t *codes, int count)
{
    return sendMessage(MLINK_INPUT, std::vector<uint8_t>(codes, codes + count));
}

void MediaLink::close(void)
{
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        clearStream();
        streamOpen = false;
        openMsg.clear();
        infoBlk.clear();
        ym.clear();
    }
    sendMessage(MLINK_CLOSE, {});
}

//--------------------------------------------------------
// ACSI side

bool MediaLink::hasStream(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return streamOpen;
}

uint32_t MediaLink::generation(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return gen;
}

std::vector<uint8_t> MediaLink::info(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    if(streamOpen && !infoBlk.empty()) {
        return infoBlk;
    }

    std::vector<uint8_t> b(MED_INFO_SIZE, 0);
    std::string msg;
    if(!streamOpen) {
        b[MED_INFO_STATE] = MED_STATE_NONE;
    } else {
        // while ce_media starts. It isn't coming when it's not configured or can't start: say so.
        bool failed = !connected && !linkMsg.empty() && linkMsg.find("Starting") == std::string::npos;
        b[MED_INFO_STATE] = failed ? MED_STATE_ERROR : MED_STATE_STARTING;
        msg = linkMsg.empty() ? (connected ? "Opening the file..." : "Starting ce_media...") : linkMsg;
    }
    msg.resize(std::min(msg.size(), (size_t) MED_INFO_MSG_SIZE - 1));
    memcpy(&b[MED_INFO_MSG], msg.data(), msg.size());
    return b;
}

std::vector<uint8_t> MediaLink::ymTable(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return ym;
}

MediaChunk MediaLink::popChunk(uint32_t g)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    if(!streamOpen || g != gen || queue.empty()) {
        return NULL;
    }
    MediaChunk c = queue.front();
    queue.pop_front();
    queueBytes -= c->size();
    if((*c)[MED_CHUNK_FLAGS] & MED_CHF_END) {
        endGiven = true;
    }
    return c;
}

bool MediaLink::ended(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return endGiven;
}

size_t MediaLink::queuedBytes(void)
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return queueBytes;
}
