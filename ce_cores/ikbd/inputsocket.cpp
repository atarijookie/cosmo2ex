// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../misc/debug.h"
#include "../misc/utils.h"
#include "../misc/global.h"
#include "../misc/cmdsockthread.h"
#include "inputsocket.h"

void InputClient::clear(void)
{
    fd = FD_EMPTY;
    helloDone = false;
    connectedMs = 0;
    bufLen = 0;
    keysDown.reset();
    memset(mouseButtonDown, 0, sizeof(mouseButtonDown));
    memset(joyAxis, 0, sizeof(joyAxis));
    memset(joyButtonDown, 0, sizeof(joyButtonDown));
    memset(gotJoy, 0, sizeof(gotJoy));
}

InputSocket::InputSocket(InputSink *sink) : sink(sink)
{
    fdListen = FD_EMPTY;

    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        clients[i].clear();
    }
}

InputSocket::~InputSocket()
{
    close();
}

bool InputSocket::open(void)
{
    std::string portStr = Utils::dotEnvValue("INPUT_TCP_PORT");
    int port = atoi(portStr.c_str());

    if(port < 1 || port > 65535) {      // not configured? no input socket, the rest works without it
        logIkbd(LOG_WARNING, "InputSocket::open - INPUT_TCP_PORT '%s' is not a valid port, input socket disabled", portStr.c_str());
        return false;
    }

    return listenOn(port, cmdSockToken());
}

bool InputSocket::listenOn(int port, const std::string& aToken)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) {
        logIkbd(LOG_ERROR, "InputSocket::listenOn - socket() failed: %s", strerror(errno));
        return false;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));    // restart right after stop - the instance lock is the command socket

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);      // loopback only - never reachable from the network
    addr.sin_port = htons(port);

    if(bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, INPUT_MAX_CLIENTS) < 0) {
        logIkbd(LOG_ERROR, "InputSocket::listenOn - failed to listen on 127.0.0.1:%d: %s", port, strerror(errno));
        ::close(fd);
        return false;
    }

    fdListen = fd;
    token = aToken;
    logIkbd(LOG_INFO, "InputSocket - listening on 127.0.0.1:%d", this->port());
    return true;
}

int InputSocket::port(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if(fdListen == FD_EMPTY || getsockname(fdListen, (struct sockaddr *) &addr, &len) != 0) {
        return -1;
    }

    return ntohs(addr.sin_port);
}

void InputSocket::close(void)
{
    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        closeClient(clients[i]);
    }

    Utils::closeFdIfOpen(fdListen);
}

int InputSocket::setFds(fd_set *fds)
{
    int maxFd = -1;

    if(fdListen != FD_EMPTY) {
        FD_SET(fdListen, fds);
        maxFd = MAX(maxFd, fdListen);
    }

    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        if(clients[i].fd != FD_EMPTY) {
            FD_SET(clients[i].fd, fds);
            maxFd = MAX(maxFd, clients[i].fd);
        }
    }

    return maxFd;
}

void InputSocket::handle(fd_set *fds)
{
    if(fdListen != FD_EMPTY && FD_ISSET(fdListen, fds)) {
        acceptClient();
    }

    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        InputClient& client = clients[i];

        if(client.fd != FD_EMPTY && FD_ISSET(client.fd, fds)) {
            readClient(client);
        }

        if(client.fd != FD_EMPTY && !client.helloDone && Utils::elapsedMs(client.connectedMs) > INPUT_HELLO_TIMEOUT_MS) {
            logIkbd(LOG_WARNING, "InputSocket - no hello in %d ms, closing connection", INPUT_HELLO_TIMEOUT_MS);
            closeClient(client);
        }
    }
}

void InputSocket::acceptClient(void)
{
    int fd = accept(fdListen, NULL, NULL);

    if(fd < 0) {
        return;
    }

    InputClient *client = NULL;
    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        if(clients[i].fd == FD_EMPTY) {
            client = &clients[i];
            break;
        }
    }

    if(!client) {                       // all slots taken - refuse the new one, keep the apps which work
        logIkbd(LOG_WARNING, "InputSocket - already %d apps connected, refusing new connection", INPUT_MAX_CLIENTS);
        ::close(fd);
        return;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    client->clear();
    client->fd = fd;
    client->connectedMs = Utils::getCurrentMs();
    logIkbd(LOG_DEBUG, "InputSocket - app connected, waiting for hello");
}

void InputSocket::closeClient(InputClient& client)
{
    if(client.fd == FD_EMPTY) {
        return;
    }

    bool wasAuthenticated = client.helloDone;

    releaseAll(client);                 // nothing stays stuck on the ST
    Utils::closeFdIfOpen(client.fd);
    client.clear();
    reportJoysticks();

    if(wasAuthenticated) {
        logIkbd(LOG_INFO, "InputSocket - app disconnected");
    }
}

void InputSocket::readClient(InputClient& client)
{
    ssize_t res = recv(client.fd, client.buf + client.bufLen, sizeof(client.buf) - client.bufLen, 0);

    if(res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }

    if(res <= 0) {                      // closed or broken
        closeClient(client);
        return;
    }

    client.bufLen += res;

    if(!client.helloDone && !handleHello(client)) {
        return;                         // waiting for the rest of hello, or connection closed
    }

    int offset = 0;
    while(client.bufLen - offset >= INPUT_EVENT_SIZE) {
        processEvent(client, client.buf + offset);
        offset += INPUT_EVENT_SIZE;
    }

    memmove(client.buf, client.buf + offset, client.bufLen - offset);   // keep the partial event for the next read
    client.bufLen -= offset;
}

bool InputSocket::handleHello(InputClient& client)
{
    uint8_t *nl = (uint8_t *) memchr(client.buf, '\n', client.bufLen);

    if(!nl) {
        if(client.bufLen >= INPUT_HELLO_MAX_LEN) {
            logIkbd(LOG_WARNING, "InputSocket - no hello line, closing connection");
            closeClient(client);
        }
        return false;
    }

    std::string line((const char *) client.buf, nl - client.buf);

    if(!helloValid(line, token)) {
        logIkbd(LOG_WARNING, "InputSocket - invalid hello (wrong protocol version or token), closing connection");
        closeClient(client);
        return false;
    }

    if(send(client.fd, "OK\n", 3, MSG_NOSIGNAL) != 3) {
        closeClient(client);
        return false;
    }

    int helloLen = (nl - client.buf) + 1;
    memmove(client.buf, client.buf + helloLen, client.bufLen - helloLen);  // events may have come right after hello
    client.bufLen -= helloLen;
    client.helloDone = true;

    logIkbd(LOG_INFO, "InputSocket - app connected");
    return true;
}

bool InputSocket::helloValid(const std::string& line, const std::string& token)
{
    std::string prefix = INPUT_HELLO_PREFIX;

    if(token.empty() || line.length() != prefix.length() + token.length() || line.compare(0, prefix.length(), prefix) != 0) {
        return false;
    }

    return cmdSockStripToken((line.substr(prefix.length()) + "\n").c_str(), token) != NULL;    // constant time token compare
}

void InputSocket::processEvent(InputClient& client, const uint8_t *ev)
{
    int type  = ev[0];
    int index = ev[1];
    int code  = (ev[2] << 8) | ev[3];
    int value = (int32_t) (((uint32_t) ev[4] << 24) | ((uint32_t) ev[5] << 16) | ((uint32_t) ev[6] << 8) | (uint32_t) ev[7]);

    switch(type) {
        case INPUT_EV_KEY:
            if(code >= INPUT_KEY_COUNT || value < 0 || value > 2) {
                return;
            }
            client.keysDown[code] = (value != 0);
            sendKey(code, value);
            break;

        case INPUT_EV_MOUSE_MOVE:
            if(code != REL_X && code != REL_Y && code != REL_WHEEL) {
                return;
            }
            sendMouse(EV_REL, code, value);
            break;

        case INPUT_EV_MOUSE_BUTTON:
            if(code > 1 || value < 0 || value > 1) {
                return;
            }
            client.mouseButtonDown[code] = (value == 1);
            sendMouse(EV_KEY, (code == 0) ? BTN_LEFT : BTN_RIGHT, value);
            break;

        case INPUT_EV_JOY_AXIS:
        case INPUT_EV_JOY_BUTTON:
            if(index >= INPUT_JOY_COUNT) {
                return;
            }

            if(type == INPUT_EV_JOY_AXIS) {
                if(code >= INPUT_JOY_AXES) {
                    return;
                }
                value = MAX(-32767, MIN(32767, value));
                client.joyAxis[index][code] = value;
                sendJoy(index, JS_EVENT_AXIS, code, value);
            } else {
                if(code >= INPUT_JOY_BUTTONS || value < 0 || value > 1) {
                    return;
                }
                client.joyButtonDown[index][code] = (value == 1);
                sendJoy(index, JS_EVENT_BUTTON, code, value);
            }

            if(!client.gotJoy[index]) {         // first event of this joystick from this app
                client.gotJoy[index] = true;
                reportJoysticks();
            }
            break;

        default:                        // unknown type - newer app, ignore
            return;
    }
}

void InputSocket::releaseAll(InputClient& client)
{
    for(int code = 0; code < INPUT_KEY_COUNT; code++) {
        if(client.keysDown[code]) {
            sendKey(code, 0);
        }
    }
    client.keysDown.reset();

    for(int btn = 0; btn < 2; btn++) {
        if(client.mouseButtonDown[btn]) {
            sendMouse(EV_KEY, (btn == 0) ? BTN_LEFT : BTN_RIGHT, 0);
            client.mouseButtonDown[btn] = false;
        }
    }

    for(int joy = 0; joy < INPUT_JOY_COUNT; joy++) {
        for(int axis = 0; axis < INPUT_JOY_AXES; axis++) {
            if(client.joyAxis[joy][axis] != 0) {
                sendJoy(joy, JS_EVENT_AXIS, axis, 0);
                client.joyAxis[joy][axis] = 0;
            }
        }

        for(int btn = 0; btn < INPUT_JOY_BUTTONS; btn++) {
            if(client.joyButtonDown[joy][btn]) {
                sendJoy(joy, JS_EVENT_BUTTON, btn, 0);
                client.joyButtonDown[joy][btn] = false;
            }
        }
    }
}

void InputSocket::reportJoysticks(void)
{
    bool joy[INPUT_JOY_COUNT] = {false, false};

    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        for(int j = 0; j < INPUT_JOY_COUNT; j++) {
            joy[j] = joy[j] || clients[i].gotJoy[j];
        }
    }

    sink->joysticksPresent(joy[0], joy[1]);
}

void InputSocket::sendKey(int code, int value)
{
    input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    sink->keyboardEvent(&ev);
}

void InputSocket::sendMouse(int type, int code, int value)
{
    input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    sink->mouseEvent(&ev);
}

void InputSocket::sendJoy(int joy, int type, int number, int value)
{
    js_event jse;
    memset(&jse, 0, sizeof(jse));
    jse.time = Utils::getCurrentMs();
    jse.type = type;
    jse.number = number;
    jse.value = value;
    sink->joystickEvent(&jse, joy);
}
