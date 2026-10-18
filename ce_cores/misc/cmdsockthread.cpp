// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/joystick.h>

#include <signal.h>
#include <pthread.h>

#include "../misc/utils.h"
#include "../misc/debug.h"
#include "../misc/global.h"
#include "../misc/statusreport.h"
#include "../misc/json.h"
#include "../discovery/discovery.h"
#include "../hdd/acsicommand/screencastacsicommand.h"

using json = nlohmann::json;

void handleGenericAction(std::string& action, json& data);
void handleSceencastAction(std::string& action, json& data, int sock, const struct sockaddr_in& from);
void handleDisksAction(std::string& action, json& data);
void handleFloppyAction(std::string& action, json& data);
void handleIkbdAction(std::string& action, json& data);
void closeFifo(bool keybNotMouse);

// Commands come from the webserver as UDP datagrams on 127.0.0.1:CMD_UDP_PORT (portable, unlike UNIX dgram sockets).
// Loopback is reachable by every local user, so each datagram starts with a secret token line: "<token>\n<json>".
// The token is regenerated on every start and stored in CMD_TOKEN_FILE (owner only), where the webserver reads it.
static std::string cmdToken;

static bool createCmdToken(void)
{
    std::string tokenPath = Utils::dotEnvValue("CMD_TOKEN_FILE");

    if(tokenPath.empty()) {
        logHdd(LOG_ERROR, "createCmdToken - CMD_TOKEN_FILE is not set");
        return false;
    }

    uint8_t rnd[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t got = (fd >= 0) ? read(fd, rnd, sizeof(rnd)) : -1;
    Utils::closeFdIfOpen(fd);

    if(got != (ssize_t) sizeof(rnd)) {
        logHdd(LOG_ERROR, "createCmdToken - failed to read /dev/urandom: %s", strerror(errno));
        return false;
    }

    char hex[2 * sizeof(rnd) + 1];
    for(size_t i = 0; i < sizeof(rnd); i++) {
        sprintf(hex + 2 * i, "%02x", rnd[i]);
    }

    Utils::mkpath(Utils::dotEnvValue("DATA_DIR").c_str(), S_IRWXU);    // the token file lives there - owner only

    if(!Utils::writeFileAtomically(tokenPath.c_str(), (const uint8_t*) hex, strlen(hex))) {
        logHdd(LOG_ERROR, "createCmdToken - failed to write %s: %s", tokenPath.c_str(), strerror(errno));
        return false;
    }

    chmod(tokenPath.c_str(), 0600);         // owner only - webserver runs as the same user and can still read it

    cmdToken = hex;
    return true;
}

// If msg starts with the valid token line, returns pointer to the json part after it, otherwise NULL.
const char* cmdSockStripToken(const char* msg, const std::string& token)
{
    const char* nl = strchr(msg, '\n');

    if(token.empty() || !nl || (size_t)(nl - msg) != token.length()) {
        return NULL;
    }

    unsigned char diff = 0;
    for(size_t i = 0; i < token.length(); i++) {    // constant time compare - don't leak how many chars matched
        diff |= (unsigned char) (msg[i] ^ token[i]);
    }

    return (diff == 0) ? (nl + 1) : NULL;
}

// The command socket is also the single instance lock: only one process can bind 127.0.0.1:CMD_UDP_PORT, and the OS
// releases it when the process exits or crashes - works the same on Linux, macOS and Windows (Cygwin).
// main() opens it before any thread starts, and it stays open until the process exits, so the lock is held during the
// whole shutdown too (e.g. while floppy images are being saved).
static int cmdSock = -1;

bool cmdSockOpen(int& err)
{
    std::string portStr = Utils::dotEnvValue("CMD_UDP_PORT");
    int port = atoi(portStr.c_str());

    logHdd(LOG_DEBUG, "cmdSockOpen - CMD_UDP_PORT = %s", portStr.c_str());

    if(port < 1 || port > 65535) {
        logHdd(LOG_ERROR, "cmdSockOpen - CMD_UDP_PORT '%s' is not a valid port", portStr.c_str());
        err = EINVAL;
        return false;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        err = errno;
        logHdd(LOG_ERROR, "cmdSockOpen - failed to create socket: %s", strerror(err));
        return false;
    }

    fcntl(sock, F_SETFD, FD_CLOEXEC);       // processes started by system() must not inherit it, they'd keep the lock after we're gone

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only - never reachable from the network
    addr.sin_port = htons(port);

    // no SO_REUSEADDR - the bind must fail when other process has this port
    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        err = errno;
        logHdd(err == EADDRINUSE ? LOG_DEBUG : LOG_ERROR, "cmdSockOpen - failed to bind socket to 127.0.0.1:%d: %s", port, strerror(err));
        close(sock);                        // don't leak the socket
        return false;
    }

    // token only after bind succeeded - a 2nd instance (which fails to bind) must not replace the token of the running one
    if(!createCmdToken()) {
        err = EIO;
        close(sock);
        return false;
    }

    cmdSock = sock;
    logHdd(LOG_DEBUG, "cmdSockOpen - 127.0.0.1:%d created, sock: %d", port, sock);
    return true;
}

void *cmdSockThreadCode(void *ptr)
{
    logHdd(LOG_INFO, "Command Socket thread starting...");
    int sock = cmdSock;         // opened by main() - see cmdSockOpen()

    if(sock < 0) {              // without socket nobody can control the app - terminate whole app with error
        logHdd(LOG_ERROR, "Command socket is not open, terminating");
        printf("Command socket is not open, terminating\n");
        fatalErrorOccurred = 1;
        sigintReceived = 1;
        return 0;
    }

    char bfr[4096];

    while(sigintReceived == 0) {
        struct timeval timeout;
        timeout.tv_sec = 1;                             // short timeout
        timeout.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        int res = select(sock + 1, &readfds, NULL, NULL, &timeout);     // wait for data or timeout here

        if(res < 0 || !FD_ISSET(sock, &readfds)) {          // if select() failed or cannot read from fd, skip rest
            continue;
        }

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t recvCnt = recvfrom(sock, bfr, sizeof(bfr) - 1, 0, (struct sockaddr *) &from, &fromLen);  // leave space for terminating zero

        if(recvCnt < 1) {                                   // nothing received?
            continue;
        }

        bfr[recvCnt] = 0;                                   // terminate string, so parse and log won't go past received data

        if((ntohl(from.sin_addr.s_addr) >> 24) != 127) {    // not from loopback? (shouldn't happen, socket is bound to 127.0.0.1)
            logHdd(LOG_WARNING, "cmdSockThreadCode: ignoring message from non-loopback address %s", inet_ntoa(from.sin_addr));
            continue;
        }

        const char* msg = cmdSockStripToken(bfr, cmdToken);

        if(!msg) {                                          // missing or wrong token? don't log the content, might be anything
            logHdd(LOG_WARNING, "cmdSockThreadCode: ignoring message with missing or invalid token");
            continue;
        }

        logHdd(LOG_DEBUG, "cmdSockThreadCode: received: %s", msg);

        // parse and handle message in one try block - json::parse() throws on invalid json, get<>() throws on wrong value type
        try {
            json data = json::parse(msg);   // try to parse the message

            if(data.contains("module") && data.contains("action")) {    // mandatory fields found?
                std::string module = data["module"].get<std::string>();
                std::string action = data["action"].get<std::string>();

                // floppy and ikbd messages go to their own logs, everything else (all, disks, screencast) to the HDD log
                int whichLog = (module == "floppy") ? LOGFILE_FDD : ((module == "ikbd") ? LOGFILE_IKBD : LOGFILE_HDD);
                Debug::out(whichLog, LOG_DEBUG, "cmdSockThreadCode: module: %s, action: %s", module.c_str(), action.c_str());

                if (module == "all") {                  // generic / all modules?
                    handleGenericAction(action, data);
                } else if (module == "disks") {         // disk modules?
                    handleDisksAction(action, data);
                } else if(module == "screencast") {
                    handleSceencastAction(action, data, sock, from);
                } else if(module == "floppy") {         // for floppy module?
                    handleFloppyAction(action, data);
                } else if(module == "ikbd") {           // for ikbd module?
                    handleIkbdAction(action, data);
                } else {                                // for uknown module?
                    logHdd(LOG_WARNING, "cmdSockThreadCode: uknown module '%s', ignoring message!", module.c_str());
                }
            } else {        // some mandatory field is missing?
                logHdd(LOG_WARNING, "cmdSockThreadCode: module or action is missing in the received data, ignoring message!");
            }
        }
        catch(const std::exception& e)      // invalid json or wrong value type - log it, don't crash
        {
            logHdd(LOG_ERROR, "cmdSockThreadCode: failed to handle message, exception: %s", e.what());
        }
        catch(...)                          // on any other exception - log it, don't crash
        {
            logHdd(LOG_ERROR, "cmdSockThreadCode: failed to handle message, unknown exception");
        }
    }

    // sock is not closed here - it's the instance lock, released by the OS when the process exits

    closeFifo(true);
    closeFifo(false);

    logHdd(LOG_INFO, "Command Socket thread terminated.");
    return 0;
}

void handleGenericAction(std::string& action, json& data)
{
    if(action == "generate_status") {
        StatusReport::createReportFiles();
    } else if(action == "set_loglevel") {
        int loglevel = 1;

        if(data.contains("loglevel")) {            // loglevel is present in message
            loglevel = data["loglevel"].get<int>();
            Debug::setLogLevel(loglevel);
        } else {
            logHdd(LOG_WARNING, "handleGenericAction: missing 'loglevel' in message, ignoring message!");
        }
    } else {
        logHdd(LOG_WARNING, "handleGenericAction: unknown action '%s', ignoring message!", action.c_str());
    }
}

int fdVirtKeyboard = FD_EMPTY;
int fdVirtMouse = FD_EMPTY;

int openFifo(bool keybNotMouse)
{
    int& fd = keybNotMouse ? fdVirtKeyboard : fdVirtMouse;
    std::string devpath = keybNotMouse ? Utils::dotEnvValue("IKBD_VIRTUAL_KEYBOARD_FILE") : Utils::dotEnvValue("IKBD_VIRTUAL_MOUSE_FILE");

    if(fd >= 0) {       // already got fd? just return it
        return fd;
    }

    std::string vdevFolder = Utils::dotEnvValue("IKBD_VIRTUAL_DEVICES_PATH");
    Utils::mkpath(vdevFolder.c_str(), S_IRWXU);     // make dir where the virtual devs will be - owner only

    // create fifo only if it's not there yet - don't recreate existing fifo, ikbd core might have it open already
    struct stat sb;
    bool isFifo = (stat(devpath.c_str(), &sb) == 0 && S_ISFIFO(sb.st_mode));

    if(!isFifo) {
        logIkbd(LOG_DEBUG, "Creating %s", devpath.c_str());

        unlink(devpath.c_str());            // try to remove if it's a file

        int err = mkfifo(devpath.c_str(), 0600);    // owner only - only ce_cores writes and reads it

        if(err < 0) {
            logIkbd(LOG_ERROR, "Could not create %s: %s", devpath.c_str(), strerror(errno));
        }
    }

    chmod(devpath.c_str(), 0600);           // also fix mode of fifo created earlier with other mode

    fd = open(devpath.c_str(), O_RDWR | O_NONBLOCK);    // we open Read/Write in order to initialize the fifo

    if(fd < 0) {
        logIkbd(LOG_ERROR, "openFifo(): open(%s) : %s", devpath.c_str(), strerror(errno));
    }

    return fd;
}

void closeFifo(bool keybNotMouse)
{
    int& fd = keybNotMouse ? fdVirtKeyboard : fdVirtMouse;

    Utils::closeFdIfOpen(fd);
}

void sendKeyboardPacket(int iKeyCode, int iState)
{
    int fd = openFifo(true);

    if(fd < 0) {                // no fifo? can't send, openFifo() logged why
        return;
    }

    input_event xEvent;
    gettimeofday(&xEvent.time, NULL);
    xEvent.type = EV_KEY;
    xEvent.code = iKeyCode;
    xEvent.value = iState;        // ev->value -- 1: down, 2: auto repeat, 0: up

    ssize_t res = write(fd, &xEvent, sizeof(xEvent));
    if(res < 0) {
        logIkbd(LOG_ERROR, "sendKeyboardPacket() write: %s", strerror(errno));
    }
}

void sendMouseButton(int iButton,int iState)
{
    if(iButton != 0 && iButton != 1) {      // only left and right buttons supported
        logIkbd(LOG_WARNING, "sendMouseButton() - unsupported button %d, ignoring", iButton);
        return;
    }

    int fd = openFifo(false);

    if(fd < 0) {                // no fifo? can't send, openFifo() logged why
        return;
    }

	ssize_t res;
    input_event xEvent;
    gettimeofday(&xEvent.time, NULL);
    xEvent.type = EV_KEY;
    xEvent.code = (iButton == 0) ? BTN_LEFT : BTN_RIGHT;

    xEvent.value = iState;

    res = write(fd, &xEvent, sizeof(xEvent));
	if(res < 0 ) {
		logIkbd(LOG_ERROR, "sendMouseButton() write: %s", strerror(errno));
	}
}

void sendMousePacket(int iX, int iY)
{
    int fd = openFifo(false);

    if(fd < 0) {                // no fifo? can't send, openFifo() logged why
        return;
    }

	ssize_t res;

    input_event ev[2];
    gettimeofday(&ev[0].time, NULL);
	memcpy(&ev[1].time, &ev[0].time, sizeof(ev[0].time));
    ev[0].type = EV_REL;
    ev[0].code = REL_X;
    ev[0].value = iX;
    ev[1].type = EV_REL;
    ev[1].code = REL_Y;
    ev[1].value = iY;
    res = write(fd, ev, sizeof(ev));

	if(res < 0) {
		logIkbd(LOG_ERROR, "sendMousePacket() write: %s", strerror(errno));
	}
}

void handleIkbdAction(std::string& action, json& data)
{
    // on ikbd settings reload
    if(action == "reload") {
        events.ikbdReloadConfig = true;
        return;
    }

    // on mouse
    if(action == "mouse") {
        /*
        example data:
            {'module': 'ikbd', 'action': 'mouse', 'type': 'relative', 'x': -1, 'y': 11}
            {'module': 'ikbd', 'action': 'mouse', 'type': 'relative', 'x': 0, 'y': 6}
            {'module': 'ikbd', 'action': 'mouse', 'type': 'buttonleft', 'state': 'down'}
            {'module': 'ikbd', 'action': 'mouse', 'type': 'buttonleft', 'state': 'up'}
            {'module': 'ikbd', 'action': 'mouse', 'type': 'buttonright', 'state': 'down'}
            {'module': 'ikbd', 'action': 'mouse', 'type': 'buttonright', 'state': 'up'}
        */

        if(data.contains("type")) {
            std::string type = data["type"].get<std::string>();

            if(type == "relative") {
                if(data.contains("x") && data.contains("y")) {
                    int x = data["x"].get<int>();
                    int y = data["y"].get<int>();

                    sendMousePacket(x, y);      // send mouse packet now
                } else {
                    logIkbd(LOG_WARNING, "handleIkbdAction: mouse - relative - missing 'x' or 'y' in message, ignoring message!");
                }
            } else if(type == "buttonleft" || type == "buttonright") {
                if(data.contains("state")) {
                    std::string stateStr = data["state"].get<std::string>();
                    int state = (stateStr == "down") ? 1 : ((stateStr == "up") ? 0 : -1);           // down -> 1, up -> 0, others: -1
                    int button = (type == "buttonright") ? 1 : ((type == "buttonleft") ? 0 : -1);   // right -> 1, left -> 0, others: -1

                    if(button == -1) {              // invalid mouse button
                        logIkbd(LOG_WARNING, "handleIkbdAction: mouse - button - invalid type in message, ignoring message!");
                    } else if(state == -1) {        // invalid state
                        logIkbd(LOG_WARNING, "handleIkbdAction: mouse - button - invalid state '%s', ignoring message!", stateStr.c_str());
                    } else {                        // valid button and state? ship it
                        sendMouseButton(button, state);
                    }
                } else {
                    logIkbd(LOG_WARNING, "handleIkbdAction: mouse - button - missing 'state' in message, ignoring message!");
                }
            } else {
                logIkbd(LOG_WARNING, "handleIkbdAction: mouse - unknown type '%s', ignoring message!", type.c_str());
            }
        } else {
            logIkbd(LOG_WARNING, "handleIkbdAction: mouse - missing 'type' in message, ignoring message!");
        }

        return;
    }

    // on keyboard
    if(action == "keyboard") {
        /*
        example data:
            {'module': 'ikbd', 'action': 'keyboard', 'type': 'pc', 'code': 35, 'state': 'down'}
            {'module': 'ikbd', 'action': 'keyboard', 'type': 'pc', 'code': 35, 'state': 'up'}
        */

        if(data.contains("code") && data.contains("state")) {
            int code = data["code"].get<int>();
            std::string stateStr = data["state"].get<std::string>();
            int state = (stateStr == "down") ? 1 : ((stateStr == "up") ? 0 : -1);   // down -> 1, up -> 0, others: -1

            if(state == -1) {       // invalid state?
                logIkbd(LOG_WARNING, "handleIkbdAction: keyboard - invalid state '%s', ignoring message!", stateStr.c_str());
            } else {                // goot state? ship it
                sendKeyboardPacket(code, state);
            }
        } else {
            logIkbd(LOG_WARNING, "handleIkbdAction: keyboard - missing 'code' or 'state' in message, ignoring message!");
        }

        return;
    }

    logIkbd(LOG_WARNING, "handleIkbdAction: unknown action '%s', ignoring message!", action.c_str());
}

// reply to 'get_frame' - the whole frame as chunk datagrams back to the sender (see SCREENCAST_CHUNK_* for format)
static void sendScreencastFrame(int sock, const struct sockaddr_in& to)
{
    static uint8_t frm[SCREENCAST_FRAME_SIZE];                                  // only command socket thread uses these
    static uint8_t chunk[SCREENCAST_CHUNK_HEADER + SCREENCAST_CHUNK_PAYLOAD];

    ScreencastAcsiCommand::frameGet(frm);                   // one consistent copy - all chunks come from the same frame

    for(size_t offset = 0; offset < SCREENCAST_FRAME_SIZE; offset += SCREENCAST_CHUNK_PAYLOAD) {
        size_t len = ScreencastAcsiCommand::frameChunk(frm, offset, chunk);

        if(sendto(sock, chunk, len, 0, (const struct sockaddr *) &to, sizeof(to)) < 0) {
            logHdd(LOG_ERROR, "sendScreencastFrame: sendto failed: %s", strerror(errno));
            return;                                         // webserver will time out and report the failure
        }
    }
}

void handleSceencastAction(std::string& action, json& data, int sock, const struct sockaddr_in& from)
{
    if(action == "get_frame") {                         // webserver wants the latest frame?
        sendScreencastFrame(sock, from);
    } else if(action == "do_screenshot") {                     // take a screenshot?
        events.doScreenShot = true;
    } else if(action == "vbl") {
        if(data.contains("value")) {                    // value is present in message
            int value = data["value"].get<int>();
            Utils::screenShotVblEnabled(value);
        } else {
            logHdd(LOG_WARNING, "handleSceencastAction: missing 'value' in message, ignoring message!");
        }
    } else {
        logHdd(LOG_WARNING, "handleSceencastAction: unknown action '%s', ignoring message!", action.c_str());
    }
}

void handleDisksAction(std::string& action, json& data)
{
    if(action == "reload_trans") {              // reload translated disks
        events.hddReloadTranslated = true;
    } else if(action == "reload_raw") {         // reload raw disks
        events.hddReloadRaw = true;
    } else {
        logHdd(LOG_WARNING, "handleDisksAction: unknown action '%s', ignoring message!", action.c_str());
    }
}

void handleFloppyAction(std::string& action, json& data)
{
    int fddAction;

    if(action == "insert") {                    // for 'insert' action
        fddAction = FDD_ACTION_INSERT;
    } else if(action == "eject") {              // for 'eject' action
        fddAction = FDD_ACTION_EJECT;
    } else {
        logFdd(LOG_WARNING, "handleFloppyAction: unknown action '%s', ignoring message!", action.c_str());
        return;
    }

    std::string mac;
    if(data.contains("mac")) {                 // get mac if you can
        mac = data["mac"].get<std::string>();
    }

    fddActionPush(fddAction, mac);              // FDD thread will handle it
}
