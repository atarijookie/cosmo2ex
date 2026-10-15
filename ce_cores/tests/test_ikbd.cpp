/*
    Tests of IKBD handling - commands from ST, data from original keyboard, USB joystick reporting.
    A socketpair stands in for the device connection: CE side is registered as client, on the test side the test writes
    what the device would send (tagged bytes) and reads what CE sends back to the ST - same socket, like the real device.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <linux/input.h>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/settings.h"
#include "misc/debug.h"
#include "chipinterface/chipinterface.h"
#include "ikbd/ikbd.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

void handleClients(Ikbd& ikbd, fd_set* pReadfds, ChipInterface* ciIkbd);     // from ikbd/coreikbd.cpp
void processInputFromAttachedDevices(Ikbd& ikbd, fd_set* pReadfds);          // from ikbd/coreikbd.cpp

struct IkbdHarness {
    std::string     settingsDir;
    ChipInterface  *ci;
    Ikbd           *ikbd;
    int             sock[2];        // [0] = test side (device), [1] = CE side, registered as client 0

    IkbdHarness(bool firstJoyIs0 = false) {
        char tmpl[] = "/tmp/ce_test_ikbd_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        settingsDir = tmpl;
        dotEnv["SETTINGS_DIR"] = settingsDir;

        Settings s;
        s.setBool("JOY_FIRST_IS_0", firstJoyIs0);

        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock) == 0);

        ci = new ChipInterface(LOGFILE_IKBD, 0, 0, 0);     // no listening socket, just the client table
        ci->clientGetByIndex(0)->fdClient = sock[1];       // device connected as client 0

        ikbd = new Ikbd(ci);
    }

    ~IkbdHarness() {
        delete ikbd;
        ci->clientGetByIndex(0)->fdClient = FD_EMPTY;
        delete ci;

        if(sock[0] >= 0) {
            close(sock[0]);
        }
        close(sock[1]);

        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + settingsDir + "'";
        system(cmd.c_str());
    }

    void deviceSends(uint8_t tag, const std::vector<uint8_t> &bytes) {
        std::vector<uint8_t> framed;
        for(uint8_t b : bytes) {                // device sends TAG + VALUE pairs
            framed.push_back(tag);
            framed.push_back(b);
        }
        REQUIRE(write(sock[0], framed.data(), framed.size()) == (ssize_t) framed.size());

        uint8_t peek;
        while(ikbd->processReceivedCommands(false, sock[1]) > 0) {         // read in pieces of max 128 bytes
            if(recv(sock[1], &peek, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {      // nothing left to read
                break;
            }
        }
    }

    void stSends(const std::vector<uint8_t> &cmd)       { deviceSends(UARTMARK_STCMD, cmd); }      // ST -> IKBD command bytes
    void keyboardSends(const std::vector<uint8_t> &d)   { deviceSends(UARTMARK_KEYBDATA, d); }     // original keyboard -> ST bytes

    std::vector<uint8_t> sentToSt(void) {
        std::vector<uint8_t> out;
        uint8_t buf[256];
        while(true) {
            ssize_t n = recv(sock[0], buf, sizeof(buf), MSG_DONTWAIT);
            if(n <= 0) {
                break;
            }
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    }

    void usbEvent(int type, int code, int value, bool keyboard = false) {
        input_event e;
        memset(&e, 0, sizeof(e));
        e.type = type;
        e.code = code;
        e.value = value;
        if(keyboard) {
            ikbd->processKeyboard(&e, false);
        } else {
            ikbd->processMouse(&e);
        }
    }

    void joystickButton(int joyNumber, int value) {
        js_event e;
        memset(&e, 0, sizeof(e));
        e.type = JS_EVENT_BUTTON;
        e.number = 0;
        e.value = value;
        ikbd->processJoystick(&e, joyNumber);
    }

    void joystickAxis(int joyNumber, int axis, int value) {
        js_event e;
        memset(&e, 0, sizeof(e));
        e.type = JS_EVENT_AXIS;
        e.number = axis;
        e.value = value;
        ikbd->processJoystick(&e, joyNumber);
    }
};

static std::vector<uint8_t> getYAxisReply(IkbdHarness &h)    // ask 'Y=0 at top or bottom?' and return the status reply
{
    h.sentToSt();                       // drop anything sent before
    h.stSends({0x80 | STCMD_SET_Y_AT_BOTTOM});
    return h.sentToSt();
}

TEST_CASE("ikbd: memory load data is not parsed as commands")
{
    IkbdHarness h;

    h.stSends({STCMD_SET_Y_AT_BOTTOM});
    std::vector<uint8_t> reply = getYAxisReply(h);
    REQUIRE(reply.size() == 8);
    REQUIRE(reply[1] == STCMD_SET_Y_AT_BOTTOM);

    // load 2 bytes, which happen to be the RESET command (0x80 0x01) - must not reset the IKBD state
    h.stSends({STCMD_MEMORY_LOAD, 0x00, 0x80, 0x02, STCMD_RESET, 0x01});

    reply = getYAxisReply(h);
    REQUIRE(reply.size() == 8);
    CHECK(reply[1] == STCMD_SET_Y_AT_BOTTOM);           // RESET would have set Y=0 back to top

    // commands after the loaded data are handled again
    h.stSends({STCMD_MEMORY_LOAD, 0x00, 0x80, 0x01, 0x55, STCMD_SET_Y_AT_TOP});
    reply = getYAxisReply(h);
    REQUIRE(reply.size() == 8);
    CHECK(reply[1] == STCMD_SET_Y_AT_TOP);
}

TEST_CASE("ikbd: memory load is handled even when it arrives in pieces")
{
    IkbdHarness h;

    h.stSends({STCMD_SET_Y_AT_BOTTOM});
    h.stSends({STCMD_MEMORY_LOAD, 0x00, 0x80});        // header not complete yet
    h.stSends({0x03, STCMD_RESET});                     // NUM = 3, 1st data byte
    h.stSends({0x01, STCMD_SET_Y_AT_TOP});              // 2nd + 3rd data byte - look like RESET and 'Y at top'

    std::vector<uint8_t> reply = getYAxisReply(h);
    REQUIRE(reply.size() == 8);
    CHECK(reply[1] == STCMD_SET_Y_AT_BOTTOM);
}

TEST_CASE("ikbd: both-joysticks reply from keyboard stays a 3 byte packet")
{
    SUBCASE("joysticks not swapped - passed through") {
        IkbdHarness h(false);
        h.stSends({STCMD_SET_JOYSTICK_EVENT_REPORTING});
        h.sentToSt();

        h.keyboardSends({KEYBDATA_JOY_BOTH, 0x01, 0x82});
        CHECK(h.sentToSt() == std::vector<uint8_t>({KEYBDATA_JOY_BOTH, 0x01, 0x82}));
    }

    SUBCASE("joysticks swapped - port values swapped, header kept") {
        IkbdHarness h(true);
        h.stSends({STCMD_SET_JOYSTICK_EVENT_REPORTING});    // joystick mode - swapping applies
        h.sentToSt();

        // used to become 0xFE 0x01 0x82: a 2-byte joystick packet + 0x82 taken as key press by ST
        h.keyboardSends({KEYBDATA_JOY_BOTH, 0x01, 0x82});
        CHECK(h.sentToSt() == std::vector<uint8_t>({KEYBDATA_JOY_BOTH, 0x82, 0x01}));
    }
}

TEST_CASE("ikbd: joystick reports only the way the ST asked for")
{
    IkbdHarness h;
    h.stSends({STCMD_SET_JOYSTICK_EVENT_REPORTING});
    h.sentToSt();

    SUBCASE("event reporting mode - change is sent") {
        h.joystickAxis(1, 0, -20000);                   // joy 1 left
        CHECK(h.sentToSt() == std::vector<uint8_t>({KEYBDATA_JOY1, JOYDIR_LEFT}));
    }

    SUBCASE("joysticks disabled - nothing is sent") {
        h.stSends({STCMD_DISABLE_JOYSTICKS});
        h.sentToSt();

        h.joystickAxis(1, 0, -20000);
        CHECK(h.sentToSt().empty());
    }

    SUBCASE("interrogation mode - nothing sent on its own, state is reported when asked") {
        h.stSends({STCMD_SET_JOYSTICK_INTERROG_MODE});
        h.sentToSt();

        h.joystickAxis(1, 1, -20000);                   // joy 1 up
        CHECK(h.sentToSt().empty());

        h.stSends({STCMD_JOYSTICK_INTERROGATION});
        std::vector<uint8_t> reply = h.sentToSt();
        REQUIRE(reply.size() == 3);
        CHECK(reply[0] == KEYBDATA_JOY_BOTH);
        CHECK(reply[2] == JOYDIR_UP);
    }

    SUBCASE("monitoring mode (not supported) - no event packets") {
        h.stSends({STCMD_SET_JOYSTICK_MONITORING, 0x01});
        h.sentToSt();

        h.joystickAxis(1, 0, 20000);
        CHECK(h.sentToSt().empty());
    }
}

TEST_CASE("ikbd: closed connection is reported, so it can be closed")
{
    IkbdHarness h;

    h.stSends({STCMD_SET_Y_AT_TOP});                    // some data first - processed normally

    close(h.sock[0]);                                   // device closed the connection
    h.sock[0] = -1;

    CHECK(h.ikbd->processReceivedCommands(false, h.sock[1]) == -1);         // used to return 0 = 'nothing now' forever
}

TEST_CASE("ikbd: client which closed the connection is removed from clients")
{
    IkbdHarness h;

    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ClientInfo *client = h.ci->clientGetByIndex(1);
    client->fdClient = sv[0];                           // connected client in slot 1
    close(sv[1]);                                       // ...which closes its end

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sv[0], &readfds);                            // select() reports EOF as readable

    handleClients(*h.ikbd, &readfds, h.ci);

    CHECK(client->fdClient == FD_EMPTY);                // closed now - otherwise select() would wake up the loop all the time
    CHECK(h.ci->clientGetByIndex(0)->fdClient == h.sock[1]);    // other client untouched
}

typedef std::vector<uint8_t> Bytes;

TEST_CASE("ikbd: fast relative mouse movement is split, not wrapped around")
{
    IkbdHarness h;
    h.sentToSt();

    h.usbEvent(EV_REL, REL_X, 200);                     // used to be sent as (uint8_t) 200 = -56
    CHECK(h.sentToSt() == Bytes({0xf8, 127, 0, 0xf8, 73, 0}));

    h.usbEvent(EV_REL, REL_X, -300);
    CHECK(h.sentToSt() == Bytes({0xf8, 0x80, 0, 0xf8, 0x80, 0, 0xf8, (uint8_t) -44, 0}));

    h.stSends({STCMD_SET_Y_AT_BOTTOM});
    h.sentToSt();
    h.usbEvent(EV_REL, REL_Y, -128);                    // inverted: +128 - used to overflow back to -128
    CHECK(h.sentToSt() == Bytes({0xf8, 0, 127, 0xf8, 0, 1}));
}

TEST_CASE("ikbd: paused output is queued and sent on resume")
{
    IkbdHarness h;
    h.stSends({STCMD_PAUSE_OUTPUT});
    h.sentToSt();

    h.usbEvent(EV_KEY, KEY_A, 1, true);                 // key press while paused
    h.usbEvent(EV_KEY, KEY_A, 0, true);
    CHECK(h.sentToSt().empty());

    h.stSends({STCMD_RESUME});                          // any command resumes
    CHECK(h.sentToSt() == Bytes({0x1e, 0x9e}));

    h.usbEvent(EV_KEY, KEY_A, 1, true);                 // and output works normally again
    CHECK(h.sentToSt() == Bytes({0x1e}));
}

TEST_CASE("ikbd: keycode mouse mode sends cursor keys and stores the deltas")
{
    IkbdHarness h;
    h.stSends({STCMD_SET_MOUSE_KEYCODE_MODE, 2, 3});
    h.sentToSt();

    h.usbEvent(EV_REL, REL_X, 5);                       // 2 steps of 2 units right, 1 unit left over
    CHECK(h.sentToSt() == Bytes({0x4d, 0xcd, 0x4d, 0xcd}));
    h.usbEvent(EV_REL, REL_X, -3);                      // 1 left over + -3 = -2 = 1 step left
    CHECK(h.sentToSt() == Bytes({0x4b, 0xcb}));
    h.usbEvent(EV_REL, REL_Y, 3);                       // 1 step down
    CHECK(h.sentToSt() == Bytes({0x50, 0xd0}));

    h.usbEvent(EV_KEY, BTN_LEFT, 1);                    // buttons are keys 0x74 / 0x75 in keycode mode
    h.usbEvent(EV_KEY, BTN_LEFT, 0);
    CHECK(h.sentToSt() == Bytes({0x74, 0xf4}));

    h.stSends({0x80 | STCMD_SET_MOUSE_KEYCODE_MODE});   // GET reports the stored deltas
    Bytes reply = h.sentToSt();
    REQUIRE(reply.size() == 8);
    CHECK(reply[1] == MOUSEMODE_KEYCODE);
    CHECK(reply[2] == 2);
    CHECK(reply[3] == 3);
}

TEST_CASE("ikbd: relative mouse threshold is applied")
{
    IkbdHarness h;
    h.stSends({STCMD_SET_MOUSE_THRESHOLD, 5, 5});
    h.sentToSt();

    h.usbEvent(EV_REL, REL_X, 3);
    CHECK(h.sentToSt().empty());                        // below threshold - accumulated
    h.usbEvent(EV_REL, REL_X, 3);
    CHECK(h.sentToSt() == Bytes({0xf8, 6, 0}));         // threshold reached - whole movement sent

    h.usbEvent(EV_REL, REL_Y, 2);
    h.usbEvent(EV_KEY, BTN_RIGHT, 1);                   // button change sends also the accumulated movement
    CHECK(h.sentToSt() == Bytes({0xf9, 0, 2}));
}

TEST_CASE("ikbd: absolute mouse position uses the mouse scale")
{
    IkbdHarness h;
    h.stSends({STCMD_SET_ABS_MOUSE_POS_REPORTING, 0x01, 0x00, 0x00, 0xc8});     // max 256 x 200
    h.stSends({STCMD_SET_MOUSE_SCALE, 2, 4});
    h.stSends({STCMD_LOAD_MOUSE_POS, 0x00, 0x00, 0x10, 0x00, 0x10});           // x = 16, y = 16
    h.sentToSt();

    h.usbEvent(EV_REL, REL_X, 5);                       // 5 units / scale 2 = 2 steps (1 unit kept)
    h.usbEvent(EV_REL, REL_X, 1);                       // + kept 1 = 1 more step
    h.usbEvent(EV_REL, REL_Y, 9);                       // 9 / 4 = 2 steps
    CHECK(h.sentToSt().empty());                        // nothing sent on movement in absolute mode

    h.stSends({STCMD_INTERROGATE_MOUSE_POS});
    CHECK(h.sentToSt() == Bytes({KEYBDATA_MOUSE_ABS, 0, 0x00, 16 + 3, 0x00, 16 + 2}));
}

TEST_CASE("ikbd: mouse buttons and joystick fire in mouse mode don't overwrite each other")
{
    IkbdHarness h;                                      // after reset: relative mouse, joystick buttons act as mouse buttons
    h.sentToSt();

    h.usbEvent(EV_KEY, BTN_LEFT, 1);                    // mouse left held
    CHECK(h.sentToSt() == Bytes({0xfa, 0, 0}));

    h.joystickButton(1, 1);                             // joy 1 fire = right button - left must stay pressed
    CHECK(h.sentToSt() == Bytes({0xfb, 0, 0}));

    h.joystickButton(1, 0);
    CHECK(h.sentToSt() == Bytes({0xfa, 0, 0}));         // used to send 0xf8 = left released too

    h.usbEvent(EV_KEY, BTN_LEFT, 0);
    h.sentToSt();

    h.joystickButton(0, 1);                             // joy 0 fire = left button held
    h.sentToSt();
    h.usbEvent(EV_REL, REL_X, 4);                       // mouse move must keep the joystick button pressed
    CHECK(h.sentToSt() == Bytes({0xfa, 4, 0}));
}

TEST_CASE("ikbd: two connected devices don't mix their data")
{
    IkbdHarness h;                                      // device A = client 0
    int b[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    h.ci->clientGetByIndex(1)->fdClient = b[1];         // device B = client 1
    h.ikbd->clientConnected(1);

    auto bSends = [&](const Bytes &framed) {
        REQUIRE(write(b[0], framed.data(), framed.size()) == (ssize_t) framed.size());
        h.ikbd->processReceivedCommands(false, b[1]);
    };
    auto bReceived = [&]() {
        Bytes out;
        uint8_t buf[256];
        ssize_t n;
        while((n = recv(b[0], buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    };

    // tag of A, then a whole command of B, then value of A - used to be one shared buffer = mixed up
    REQUIRE(write(h.sock[0], "\xAA", 1) == 1);
    h.ikbd->processReceivedCommands(false, h.sock[1]);
    bSends({UARTMARK_STCMD, 0x80 | STCMD_SET_Y_AT_BOTTOM});
    REQUIRE(write(h.sock[0], "\x8f", 1) == 1);
    h.ikbd->processReceivedCommands(false, h.sock[1]);

    Bytes replyA = h.sentToSt(), replyB = bReceived();
    CHECK(replyA.size() == 8);                          // each got exactly its own GET reply
    CHECK(replyB.size() == 8);

    // keyboard data of A's own keyboard goes only to A's ST
    h.keyboardSends({0x1e});
    CHECK(h.sentToSt() == Bytes({0x1e}));
    CHECK(bReceived().empty());

    // USB keyboard goes to both
    h.usbEvent(EV_KEY, KEY_A, 1, true);
    CHECK(h.sentToSt() == Bytes({0x1e}));
    CHECK(bReceived() == Bytes({0x1e}));

    h.ci->clientGetByIndex(1)->fdClient = FD_EMPTY;
    close(b[0]);
    close(b[1]);
}

TEST_CASE("ikbd: hotkey key counters don't drift")
{
    IkbdHarness h;
    Settings s;
    bool before = s.getBool("JOY_FIRST_IS_0", false);

    SUBCASE("release without press doesn't make the counter negative") {
        h.usbEvent(EV_KEY, KEY_LEFTCTRL, 0, true);      // e.g. CTRL held when keyboard was found
        h.usbEvent(EV_KEY, KEY_LEFTCTRL, 1, true);      // now CTRL + SHIFT + F11 = swap joysticks
        h.usbEvent(EV_KEY, KEY_LEFTSHIFT, 1, true);
        h.usbEvent(EV_KEY, KEY_F11, 1, true);
        CHECK(s.getBool("JOY_FIRST_IS_0", false) != before);
    }

    SUBCASE("keys held when keyboard goes away don't stay pressed") {
        h.usbEvent(EV_KEY, KEY_LEFTCTRL, 1, true);      // CTRL held, then keyboard unplugged - release never comes
        h.ikbd->deinitDev(INTYPE_KEYBOARD);

        h.usbEvent(EV_KEY, KEY_LEFTSHIFT, 1, true);     // SHIFT + F11 alone must not act as the CTRL + SHIFT + F11 hotkey
        h.usbEvent(EV_KEY, KEY_F11, 1, true);
        CHECK(s.getBool("JOY_FIRST_IS_0", false) == before);
    }
}

// virtual devices dir with FIFOs, like the one for the web API (IKBD_VIRTUAL_DEVICES_PATH)
struct VdevDir {
    std::string dir;

    VdevDir(const std::string &subdir = "") {
        char tmpl[] = "/tmp/ce_test_vdev_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        if(!subdir.empty()) {
            dir += "/" + subdir;
            REQUIRE(mkdir(dir.c_str(), 0700) == 0);
        }
        REQUIRE(mkfifo((dir + "/kbd").c_str(), 0600) == 0);

        dotEnv["IKBD_VIRTUAL_DEVICES_PATH"]  = dir;
        dotEnv["IKBD_VIRTUAL_MOUSE_FILE"]    = dir + "/mouse";
        dotEnv["IKBD_VIRTUAL_KEYBOARD_FILE"] = dir + "/kbd";
    }

    ~VdevDir() {
        dotEnv.erase("IKBD_VIRTUAL_DEVICES_PATH");
        dotEnv.erase("IKBD_VIRTUAL_MOUSE_FILE");
        dotEnv.erase("IKBD_VIRTUAL_KEYBOARD_FILE");
        std::string top = dir.substr(0, strlen("/tmp/ce_test_vdev_XXXXXX"));
        std::string cmd = "rm -rf '" + top + "'";
        system(cmd.c_str());
    }
};

static void processDeviceInput(IkbdHarness &h, int index)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    int fd = h.ikbd->getFdByIndex(index);
    REQUIRE(fd >= 0);
    FD_SET(fd, &readfds);
    processInputFromAttachedDevices(*h.ikbd, &readfds);
}

TEST_CASE("ikbd: virtual keyboard - split events are put together, FIFO is reopened after writer closes")
{
    VdevDir vdev;
    IkbdHarness h;
    h.ikbd->findVirtualDevices();
    REQUIRE(h.ikbd->getFdByIndex(INTYPE_VDEVKEYBOARD) >= 0);
    h.sentToSt();

    input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = KEY_A;
    ev.value = 1;
    const uint8_t *raw = (const uint8_t *) &ev;
    int half = sizeof(ev) / 2;

    int writer = open((vdev.dir + "/kbd").c_str(), O_WRONLY);
    REQUIRE(writer >= 0);

    REQUIRE(write(writer, raw, half) == half);          // first half of event only
    processDeviceInput(h, INTYPE_VDEVKEYBOARD);
    CHECK(h.sentToSt().empty());                        // used to be processed as a whole (garbage) event

    REQUIRE(write(writer, raw + half, sizeof(ev) - half) == (ssize_t) (sizeof(ev) - half));
    processDeviceInput(h, INTYPE_VDEVKEYBOARD);
    CHECK(h.sentToSt() == Bytes({0x1e}));               // whole event now - key A pressed

    close(writer);                                      // web server closes its end
    processDeviceInput(h, INTYPE_VDEVKEYBOARD);         // EOF
    REQUIRE(h.ikbd->getFdByIndex(INTYPE_VDEVKEYBOARD) >= 0);    // reopened (used to stay closed forever)

    writer = open((vdev.dir + "/kbd").c_str(), O_WRONLY);       // next writer works again
    REQUIRE(writer >= 0);
    ev.value = 0;
    REQUIRE(write(writer, raw, sizeof(ev)) == (ssize_t) sizeof(ev));
    processDeviceInput(h, INTYPE_VDEVKEYBOARD);
    CHECK(h.sentToSt() == Bytes({0x9e}));               // key A released

    close(writer);
    h.ikbd->closeDevs();
}

TEST_CASE("ikbd: device path longer than devPath is refused, not overflowing")
{
    VdevDir vdev(std::string(250, 'x'));                // whole path to FIFO > 255 chars
    REQUIRE(vdev.dir.size() + 4 > 255);

    IkbdHarness h;
    h.ikbd->findVirtualDevices();
    CHECK(h.ikbd->getFdByIndex(INTYPE_VDEVKEYBOARD) == -1);
    h.ikbd->closeDevs();
}

TEST_CASE("ikbd: keyboard joystick - releasing one of two opposite keys keeps the other direction")
{
    IkbdHarness h;
    Settings s;
    s.setBool("KEYBOARD_JOY1", true);                   // joy 1 on cursor keys + RSHIFT (default keys)
    h.ikbd->loadSettings();
    h.stSends({STCMD_SET_JOYSTICK_EVENT_REPORTING});
    h.sentToSt();

    h.usbEvent(EV_KEY, KEY_UP, 1, true);
    CHECK(h.sentToSt() == Bytes({KEYBDATA_JOY1, JOYDIR_UP}));
    h.usbEvent(EV_KEY, KEY_DOWN, 1, true);              // both held - last pressed wins
    CHECK(h.sentToSt() == Bytes({KEYBDATA_JOY1, JOYDIR_DOWN}));
    h.usbEvent(EV_KEY, KEY_UP, 0, true);                // UP released, DOWN still held - stays DOWN (used to center)
    CHECK(h.sentToSt().empty());
    h.usbEvent(EV_KEY, KEY_DOWN, 0, true);
    CHECK(h.sentToSt() == Bytes({KEYBDATA_JOY1, 0}));

    h.usbEvent(EV_KEY, KEY_LEFT, 1, true);
    h.usbEvent(EV_KEY, KEY_RIGHT, 1, true);
    h.usbEvent(EV_KEY, KEY_RIGHT, 0, true);             // RIGHT released, LEFT still held - back to LEFT
    CHECK(h.sentToSt() == Bytes({KEYBDATA_JOY1, JOYDIR_LEFT, KEYBDATA_JOY1, JOYDIR_RIGHT, KEYBDATA_JOY1, JOYDIR_LEFT}));
}
