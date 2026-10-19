// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef _INPUTSOCKET_H_
#define _INPUTSOCKET_H_

#include <stdint.h>
#include <string>
#include <bitset>
#include <sys/select.h>

#include "linuxinput.h"     // input_event, js_event, KEY_* - Linux codes on all systems

/*
    Input socket - entry point for keyboard, mouse and joystick events from an external app, e.g. a small desktop app
    which captures the PC's keyboard / mouse / game controllers (for systems without Linux evdev devices: Windows, macOS).
    The web UI uses it too (through the webserver). The events go to the same Ikbd handlers as events from real (evdev) devices.

    TCP on 127.0.0.1:INPUT_TCP_PORT (loopback only). Up to INPUT_MAX_CLIENTS apps at the same time (e.g. the web UI via
    the webserver + a desktop app), each with its own state. A connection which doesn't send hello in INPUT_HELLO_TIMEOUT_MS
    is closed, so stray connections can't take all the slots.

    1. app sends hello line:    "CEINPUT 1 <token>\n"
                                1 = protocol version, token = content of CMD_TOKEN_FILE (owner only, regenerated on each
                                ce_cores start - the app runs as the same user as ce_cores, so it can read it)
    2. ce_cores answers:        "OK\n"      - on wrong hello the connection is closed without answer
    3. app sends events, INPUT_EVENT_SIZE (8) bytes each, big endian:
            byte 0      type
            byte 1      index       joystick 0 / 1, otherwise 0
            byte 2-3    code        uint16
            byte 4-7    value       int32

        type                        code                                        value
        1 INPUT_EV_KEY              Linux key code (KEY_* from                  1 down, 0 up, 2 auto repeat
                                    linux/input-event-codes.h, like the web UI)
        2 INPUT_EV_MOUSE_MOVE       0 X, 1 Y, 8 wheel (REL_X / REL_Y / REL_WHEEL)  relative movement, like Linux evdev:
                                                                                   + = right / down / wheel away from user
        3 INPUT_EV_MOUSE_BUTTON     0 left, 1 right                             1 down, 0 up
        4 INPUT_EV_JOY_AXIS         axis number (0 X, 1 Y)                      -32767 .. 32767, like Linux joystick API:
                                                                                   - = left / up
        5 INPUT_EV_JOY_BUTTON       button number                               1 down, 0 up

        Unknown types are ignored, so newer apps can still talk to older ce_cores.

    On disconnect, all keys and buttons still held by that app are released and its joysticks centered,
    so nothing stays stuck on the ST when the app quits or crashes.

    Mouse counts as present (Ikbd::gotUsbMouse()) while mouse events came in the last 10 seconds, joystick 0 / 1 from
    the first event of that joystick until the app disconnects.
*/

#define INPUT_HELLO_PREFIX      "CEINPUT 1 "
#define INPUT_HELLO_MAX_LEN     64
#define INPUT_HELLO_TIMEOUT_MS  2000
#define INPUT_MAX_CLIENTS       4
#define INPUT_EVENT_SIZE        8

#define INPUT_EV_KEY            1
#define INPUT_EV_MOUSE_MOVE     2
#define INPUT_EV_MOUSE_BUTTON   3
#define INPUT_EV_JOY_AXIS       4
#define INPUT_EV_JOY_BUTTON     5

#define INPUT_KEY_COUNT         1024    // key codes tracked for release on disconnect
#define INPUT_JOY_COUNT         2
#define INPUT_JOY_AXES          8       // like JOYAXIS in ikbd.h
#define INPUT_JOY_BUTTONS       12      // like JOYBUTTONS in ikbd.h

// where InputSocket delivers the events - Ikbd in ce_cores, a recorder in tests
class InputSink
{
public:
    virtual ~InputSink() { }
    virtual void keyboardEvent(input_event *ev) = 0;
    virtual void mouseEvent(input_event *ev) = 0;
    virtual void joystickEvent(js_event *jse, int joyNumber) = 0;
    virtual void joysticksPresent(bool joy0, bool joy1) = 0;           // which joysticks some app is sending now
};

// one connected app
struct InputClient
{
    int         fd;
    bool        helloDone;
    uint32_t    connectedMs;            // Utils::getCurrentMs() when connected - for hello timeout

    uint8_t     buf[INPUT_HELLO_MAX_LEN + INPUT_EVENT_SIZE];
    int         bufLen;

    // what the app holds now - released on disconnect
    std::bitset<INPUT_KEY_COUNT> keysDown;
    bool        mouseButtonDown[2];
    int         joyAxis[INPUT_JOY_COUNT][INPUT_JOY_AXES];
    bool        joyButtonDown[INPUT_JOY_COUNT][INPUT_JOY_BUTTONS];
    bool        gotJoy[INPUT_JOY_COUNT];    // app sent events of this joystick since it connected

    void clear(void);
};

class InputSocket
{
public:
    InputSocket(InputSink *sink);
    ~InputSocket();

    bool open(void);                        // start listening on 127.0.0.1:INPUT_TCP_PORT, apps authenticate with cmdSockToken()
    bool listenOn(int port, const std::string& token);     // port 0 = any free port (tests)
    int  port(void);                        // port it listens on, -1 if not listening
    void close(void);

    int  setFds(fd_set *fds);               // add own fds to fds, returns highest fd (-1 if none)
    void handle(fd_set *fds);               // accept / read whatever select() reported in fds, drop clients without hello in time

    // protocol - public for tests
    static bool helloValid(const std::string& line, const std::string& token);     // line without "\n"
    void processEvent(InputClient& client, const uint8_t *ev);     // one INPUT_EVENT_SIZE event
    void releaseAll(InputClient& client);   // release everything the app holds, as on disconnect

private:
    InputSink   *sink;
    int         fdListen;
    std::string token;
    InputClient clients[INPUT_MAX_CLIENTS];

    void acceptClient(void);
    void closeClient(InputClient& client);
    void readClient(InputClient& client);
    bool handleHello(InputClient& client);  // false = hello not complete yet, or invalid and connection closed
    void reportJoysticks(void);

    void sendKey(int code, int value);
    void sendMouse(int type, int code, int value);
    void sendJoy(int joy, int type, int number, int value);
};

#endif
