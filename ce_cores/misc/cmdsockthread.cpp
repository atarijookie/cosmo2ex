// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/socket.h>
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

using json = nlohmann::json;

void handleGenericAction(std::string& action, json& data);
void handleSceencastAction(std::string& action, json& data);
void handleDisksAction(std::string& action, json& data);
void handleFloppyAction(std::string& action, json& data);
void handleIkbdAction(std::string& action, json& data);
void closeFifo(bool keybNotMouse);

int createRecvSocket(const char* dotEnvKey)
{
    std::string sockPath = Utils::dotEnvValue(dotEnvKey);

    logHdd(LOG_DEBUG, "createRecvSocket - %s = %s", dotEnvKey, sockPath.c_str());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    if(sockPath.empty() || sockPath.length() >= sizeof(addr.sun_path)) {     // no path or path too long for sun_path?
        logHdd(LOG_ERROR, "createRecvSocket - %s - socket path '%s' is empty or longer than %d chars", dotEnvKey, sockPath.c_str(), (int) sizeof(addr.sun_path) - 1);
        return -1;
    }

    // create a UNIX DGRAM socket
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (sock < 0) {
        logHdd(LOG_ERROR, "createRecvSocket - failed to create socket: %s", strerror(errno));
        return -1;
    }

    unlink(sockPath.c_str());               // delete sock file if exists

    strcpy(addr.sun_path, sockPath.c_str());    // fits, length checked above
    addr.sun_family = AF_UNIX;

    int res = bind(sock, (struct sockaddr *) &addr, strlen(addr.sun_path) + sizeof(addr.sun_family));
    if (res < 0) {
        logHdd(LOG_ERROR, "createRecvSocket - failed to bind socket to %s: %s", sockPath.c_str(), strerror(errno));
        close(sock);                        // don't leak the socket
        return -1;
    }

    chmod(addr.sun_path, 0600);             // owner only - webserver runs as the same user and can still send commands

    logHdd(LOG_DEBUG, "createRecvSocket - %s created, sock: %d", sockPath.c_str(), sock);
    return sock;
}

void *cmdSockThreadCode(void *ptr)
{
    logHdd(LOG_INFO, "Command Socket thread starting...");
    int sock = createRecvSocket("CMD_SOCK_PATH");

    if(sock < 0) {              // without socket nobody can control the app - terminate whole app with error
        logHdd(LOG_ERROR, "Could not create command socket, terminating");
        printf("Could not create command socket, terminating\n");
        fatalErrorOccurred = 1;
        sigintReceived = 1;
        return 0;
    }

    char bfr[1024];

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

        ssize_t recvCnt = recv(sock, bfr, sizeof(bfr) - 1, 0);  // receive now, leave space for terminating zero

        if(recvCnt < 1) {                                   // nothing received?
            continue;
        }

        bfr[recvCnt] = 0;                                   // terminate string, so parse and log won't go past received data

        logHdd(LOG_DEBUG, "cmdSockThreadCode: received: %s", bfr);

        // parse and handle message in one try block - json::parse() throws on invalid json, get<>() throws on wrong value type
        try {
            json data = json::parse(bfr);   // try to parse the message

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
                    handleSceencastAction(action, data);
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

    close(sock);

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

void handleSceencastAction(std::string& action, json& data)
{
    if(action == "do_screenshot") {                     // take a screenshot?
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
