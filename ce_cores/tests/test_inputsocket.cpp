/*
    Tests of the input socket protocol from ikbd/inputsocket.cpp - hello check, event decoding, release on disconnect
*/
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "doctest.h"

#include "ikbd/inputsocket.h"

// records what InputSocket delivers, as "k <code> <value>", "m <type> <code> <value>", "j <joy> <type> <number> <value>"
class RecordingSink : public InputSink
{
public:
    std::vector<std::string> events;
    bool joy0 = false, joy1 = false;
    int presenceReports = 0;

    void keyboardEvent(input_event *ev) { add("k %d %d", ev->code, ev->value, 0, 0); }
    void mouseEvent(input_event *ev) { add("m %d %d %d", ev->type, ev->code, ev->value, 0); }
    void joystickEvent(js_event *jse, int joyNumber) { add("j %d %d %d %d", joyNumber, jse->type, jse->number, jse->value); }
    void joysticksPresent(bool j0, bool j1) { joy0 = j0; joy1 = j1; presenceReports++; }

private:
    void add(const char *fmt, int a, int b, int c, int d) {
        char s[64];
        snprintf(s, sizeof(s), fmt, a, b, c, d);
        events.push_back(s);
    }
};

static std::string evBytes(int type, int index, int code, int value)
{
    uint8_t b[INPUT_EVENT_SIZE] = {(uint8_t) type, (uint8_t) index, (uint8_t) (code >> 8), (uint8_t) code,
                                   (uint8_t) (value >> 24), (uint8_t) (value >> 16), (uint8_t) (value >> 8), (uint8_t) value};
    return std::string((const char *) b, sizeof(b));
}

// one app, processing events directly (no socket)
struct Direct {
    RecordingSink sink;
    InputSocket is;
    InputClient client;

    Direct() : is(&sink) { client.clear(); }
};

static void ev(Direct& d, int type, int index, int code, int value)
{
    std::string b = evBytes(type, index, code, value);
    d.is.processEvent(d.client, (const uint8_t *) b.data());
}

TEST_CASE("InputSocket::helloValid - needs protocol 1 and exact token")
{
    std::string token = "0123456789abcdef0123456789abcdef";
    CHECK(InputSocket::helloValid("CEINPUT 1 " + token, token));
    CHECK_FALSE(InputSocket::helloValid("CEINPUT 2 " + token, token));                     // other protocol version
    CHECK_FALSE(InputSocket::helloValid("CEINPUT 1 " + token.substr(1), token));           // too short
    CHECK_FALSE(InputSocket::helloValid("CEINPUT 1 " + token + "0", token));               // too long
    CHECK_FALSE(InputSocket::helloValid("CEINPUT 1 1123456789abcdef0123456789abcdef", token));  // one char differs
    CHECK_FALSE(InputSocket::helloValid("CEINPUT 1 ", ""));                                // no token yet - reject everything
}

TEST_CASE("InputSocket - events go to the sink as evdev / joystick events")
{
    Direct d;
    RecordingSink& sink = d.sink;

    ev(d, INPUT_EV_KEY, 0, KEY_A, 1);
    ev(d, INPUT_EV_KEY, 0, KEY_A, 0);
    ev(d, INPUT_EV_MOUSE_MOVE, 0, REL_X, -5);
    ev(d, INPUT_EV_MOUSE_MOVE, 0, REL_Y, 300);
    ev(d, INPUT_EV_MOUSE_MOVE, 0, REL_WHEEL, 1);
    ev(d, INPUT_EV_MOUSE_BUTTON, 0, 1, 1);
    ev(d, INPUT_EV_JOY_AXIS, 1, 0, -32767);
    ev(d, INPUT_EV_JOY_BUTTON, 0, 0, 1);

    std::vector<std::string> want = {
        "k 30 1", "k 30 0",
        "m 2 0 -5", "m 2 1 300", "m 2 8 1",
        "m 1 273 1",                // EV_KEY BTN_RIGHT
        "j 1 2 0 -32767",           // joy 1, JS_EVENT_AXIS
        "j 0 1 0 1",                // joy 0, JS_EVENT_BUTTON
    };
    CHECK(sink.events == want);
}

TEST_CASE("InputSocket - invalid and unknown events are ignored")
{
    Direct d;
    RecordingSink& sink = d.sink;

    ev(d, 99, 0, 0, 1);                                    // unknown type - newer app
    ev(d, INPUT_EV_KEY, 0, KEY_A, 3);                      // invalid key state
    ev(d, INPUT_EV_KEY, 0, INPUT_KEY_COUNT, 1);            // key code out of range
    ev(d, INPUT_EV_MOUSE_MOVE, 0, 5, 1);                   // not X / Y / wheel
    ev(d, INPUT_EV_MOUSE_BUTTON, 0, 2, 1);                 // middle button not supported
    ev(d, INPUT_EV_JOY_AXIS, 2, 0, 100);                   // only joysticks 0 and 1
    ev(d, INPUT_EV_JOY_BUTTON, 0, INPUT_JOY_BUTTONS, 1);   // button out of range

    CHECK(sink.events.empty());
    CHECK(sink.presenceReports == 0);
}

TEST_CASE("InputSocket - joystick axis is clamped to -32767 .. 32767")
{
    Direct d;
    RecordingSink& sink = d.sink;

    ev(d, INPUT_EV_JOY_AXIS, 0, 1, -40000);
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0] == "j 0 2 1 -32767");
}

TEST_CASE("InputSocket::releaseAll - releases only what is still held, centers joysticks")
{
    Direct d;
    RecordingSink& sink = d.sink;

    ev(d, INPUT_EV_KEY, 0, KEY_LEFTSHIFT, 1);          // held
    ev(d, INPUT_EV_KEY, 0, KEY_A, 1);
    ev(d, INPUT_EV_KEY, 0, KEY_A, 0);                  // released already
    ev(d, INPUT_EV_MOUSE_BUTTON, 0, 0, 1);             // left held
    ev(d, INPUT_EV_JOY_AXIS, 0, 0, 32767);             // joy 0 right
    ev(d, INPUT_EV_JOY_AXIS, 0, 1, 0);                 // joy 0 Y centered already
    ev(d, INPUT_EV_JOY_BUTTON, 1, 0, 1);               // joy 1 fire held

    sink.events.clear();
    d.is.releaseAll(d.client);

    std::vector<std::string> want = {
        "k 42 0",                   // left shift up
        "m 1 272 0",                // BTN_LEFT up
        "j 0 2 0 0",                // joy 0 X centered
        "j 1 1 0 0",                // joy 1 fire up
    };
    CHECK(sink.events == want);

    sink.events.clear();
    d.is.releaseAll(d.client);      // nothing held anymore
    CHECK(sink.events.empty());
}

TEST_CASE("InputSocket - joystick presence is reported on first event of that joystick")
{
    Direct d;
    RecordingSink& sink = d.sink;

    ev(d, INPUT_EV_KEY, 0, KEY_A, 1);                   // keyboard and mouse don't change joystick presence
    ev(d, INPUT_EV_MOUSE_MOVE, 0, REL_X, 1);
    CHECK(sink.presenceReports == 0);

    ev(d, INPUT_EV_JOY_BUTTON, 1, 0, 1);
    CHECK(sink.presenceReports == 1);                   // values are checked over TCP - they come from the connected apps

    ev(d, INPUT_EV_JOY_BUTTON, 1, 0, 0);                // no change, no report
    CHECK(sink.presenceReports == 1);
}

//----------------------------------------------------------------------------
// over real sockets

static const char *TOKEN = "0123456789abcdef0123456789abcdef";

struct Server {
    RecordingSink sink;
    InputSocket is;

    Server() : is(&sink) { REQUIRE(is.listenOn(0, TOKEN)); }

    void pump(int ms = 50) {                            // run select() + handle() like the ikbd thread does
        for(int i = 0; i < ms / 10; i++) {
            fd_set fds;
            FD_ZERO(&fds);
            int maxFd = is.setFds(&fds);
            struct timeval tv = {0, 10000};
            if(select(maxFd + 1, &fds, NULL, NULL, &tv) > 0) {
                is.handle(&fds);
            }
        }
    }

    int connect(void) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(is.port());
        REQUIRE(::connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
        pump();
        return fd;
    }

    int connectWithHello(void) {
        int fd = connect();
        sendStr(fd, std::string(INPUT_HELLO_PREFIX) + TOKEN + "\n");
        pump();
        REQUIRE(recvStr(fd) == "OK\n");
        return fd;
    }

    static void sendStr(int fd, const std::string& s) { REQUIRE(send(fd, s.data(), s.size(), 0) == (ssize_t) s.size()); }

    static std::string recvStr(int fd) {                // what came, "" if connection closed
        struct timeval tv = {0, 200000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char b[64];
        ssize_t n = recv(fd, b, sizeof(b), 0);
        return (n > 0) ? std::string(b, n) : std::string();
    }
};

TEST_CASE("InputSocket over TCP - wrong hello closes the connection, events before hello do nothing")
{
    Server srv;
    int fd = srv.connect();
    Server::sendStr(fd, std::string("CEINPUT 1 ") + std::string(32, '0') + "\n" + evBytes(INPUT_EV_KEY, 0, KEY_A, 1));
    srv.pump();

    CHECK(Server::recvStr(fd) == "");                   // closed without answer
    CHECK(srv.sink.events.empty());
    close(fd);
}

TEST_CASE("InputSocket over TCP - event right after hello, event split across sends")
{
    Server srv;
    int fd = srv.connect();
    Server::sendStr(fd, std::string(INPUT_HELLO_PREFIX) + TOKEN + "\n" + evBytes(INPUT_EV_KEY, 0, KEY_A, 1));  // same packet
    srv.pump();
    REQUIRE(Server::recvStr(fd) == "OK\n");

    std::string e = evBytes(INPUT_EV_MOUSE_MOVE, 0, REL_X, -7);
    Server::sendStr(fd, e.substr(0, 3));
    srv.pump();
    Server::sendStr(fd, e.substr(3));
    srv.pump();

    std::vector<std::string> want = {"k 30 1", "m 2 0 -7"};
    CHECK(srv.sink.events == want);
    close(fd);
    srv.pump();
}

TEST_CASE("InputSocket over TCP - several apps at once, disconnect releases only keys of that app")
{
    Server srv;
    int web = srv.connectWithHello();
    int app = srv.connectWithHello();                   // second app doesn't kick out the first one

    Server::sendStr(web, evBytes(INPUT_EV_KEY, 0, KEY_LEFTSHIFT, 1));
    Server::sendStr(app, evBytes(INPUT_EV_KEY, 0, KEY_A, 1));
    srv.pump();
    srv.sink.events.clear();

    close(app);                                         // app quits while holding A
    srv.pump();
    std::vector<std::string> want = {"k 30 0"};         // only A released, web UI still holds LSHIFT
    CHECK(srv.sink.events == want);

    Server::sendStr(web, evBytes(INPUT_EV_KEY, 0, KEY_B, 1));   // web UI still works
    srv.pump();
    CHECK(srv.sink.events.back() == "k 48 1");

    srv.sink.events.clear();
    close(web);
    srv.pump();
    std::vector<std::string> wantWeb = {"k 42 0", "k 48 0"};    // LSHIFT and B released when web UI disconnects
    CHECK(srv.sink.events == wantWeb);
}

TEST_CASE("InputSocket over TCP - joystick present while some app sends it, gone when that app disconnects")
{
    Server srv;
    int a = srv.connectWithHello();
    int b = srv.connectWithHello();

    Server::sendStr(a, evBytes(INPUT_EV_JOY_AXIS, 0, 0, 100));
    Server::sendStr(b, evBytes(INPUT_EV_JOY_BUTTON, 1, 0, 1));
    srv.pump();
    CHECK(srv.sink.joy0);
    CHECK(srv.sink.joy1);

    close(b);
    srv.pump();
    CHECK(srv.sink.joy0);
    CHECK_FALSE(srv.sink.joy1);

    close(a);
    srv.pump();
    CHECK_FALSE(srv.sink.joy0);
}

TEST_CASE("InputSocket over TCP - connection over the limit is refused")
{
    Server srv;
    std::vector<int> fds;
    for(int i = 0; i < INPUT_MAX_CLIENTS; i++) {
        fds.push_back(srv.connectWithHello());
    }

    int extra = srv.connect();
    Server::sendStr(extra, std::string(INPUT_HELLO_PREFIX) + TOKEN + "\n");
    srv.pump();
    CHECK(Server::recvStr(extra) == "");                // closed, no OK
    close(extra);

    close(fds[0]);                                      // slot freed - next one gets in
    srv.pump();
    int next = srv.connectWithHello();
    close(next);
    for(size_t i = 1; i < fds.size(); i++) {
        close(fds[i]);
    }
    srv.pump();
}
