// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of the browser module (HOSTMOD_BROWSER) - the host side of CE_BROWS.PRG. The commands are what CE_BROWS.PRG
// sends; ce_browser is played by a socket in the test, connected to the port BrowserLink listens on.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/utils.h"
#include "hdd/acsidatatrans.h"
#include "hdd/acsicommand/screencastacsicommand.h"
#include "hdd/browser/browsermodule.h"
#include "hdd/browser/browser_commands.h"
#include "hdd/browser/browserlink_protocol.h"
#include "hdd/browser/browserhistory.h"

namespace {

// instead of the device: gives the module the data the ST would send, keeps the reply and the status
class BrwFakeDataTrans : public AcsiDataTrans
{
public:
    std::vector<uint8_t> fromSt;
    std::vector<uint8_t> reply;
    uint8_t lastStatus = 0xff;

    bool recvData(uint8_t *data, uint32_t cnt) {
        memset(data, 0, cnt);
        memcpy(data, fromSt.data(), fromSt.size() < cnt ? fromSt.size() : cnt);
        return true;
    }

    void setStatus(uint8_t stat) {
        lastStatus = stat;
        AcsiDataTrans::setStatus(stat);
    }

    void sendDataAndStatus(bool fromRetryModule = false) {
        (void) fromRetryModule;
        reply.assign(buffer, buffer + count);
    }
};

bool waitFor(std::function<bool()> cond, int ms = 2000)
{
    for(int i=0; i<ms / 10; i++) {
        if(cond()) {
            return true;
        }
        Utils::sleepMs(10);
    }
    return cond();
}

// plays ce_browser: connects to the link, reads and sends messages
struct FakeBrowser {
    int fd = -1;
    std::vector<uint8_t> rx;

    ~FakeBrowser() { if(fd >= 0) close(fd); }

    bool connectTo(uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        return connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0;
    }

    void send(uint8_t type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> m(BLINK_HEADER_SIZE + payload.size());
        m[0] = type;
        Utils::storeDword(&m[1], payload.size());
        if(!payload.empty()) {
            memcpy(&m[BLINK_HEADER_SIZE], payload.data(), payload.size());
        }
        size_t sent = 0;
        while(sent < m.size()) {
            ssize_t r = ::send(fd, m.data() + sent, m.size() - sent, 0);
            REQUIRE(r > 0);
            sent += r;
        }
    }

    // next message within timeout: type, payload; false if none
    bool recv(uint8_t &type, std::vector<uint8_t> &payload, int timeoutMs = 2000) {
        for(int waited = 0; ; waited += 10) {
            if(rx.size() >= BLINK_HEADER_SIZE) {
                uint32_t len = Utils::getDword(&rx[1]);
                if(rx.size() >= BLINK_HEADER_SIZE + len) {
                    type = rx[0];
                    payload.assign(rx.begin() + BLINK_HEADER_SIZE, rx.begin() + BLINK_HEADER_SIZE + len);
                    rx.erase(rx.begin(), rx.begin() + BLINK_HEADER_SIZE + len);
                    return true;
                }
            }
            if(waited >= timeoutMs) {
                return false;
            }
            struct pollfd p = { fd, POLLIN, 0 };
            if(poll(&p, 1, 10) > 0) {
                uint8_t buf[65536];
                ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                if(r <= 0) {
                    return false;
                }
                rx.insert(rx.end(), buf, buf + r);
            }
        }
    }

    // next message of this type (others skipped)
    std::vector<uint8_t> expect(uint8_t wantType) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while(recv(type, payload)) {
            if(type == wantType) {
                return payload;
            }
        }
        FAIL("message " << (int) wantType << " didn't come");
        return {};
    }

    // frame with pixel bytes counting up from 'first' - so each row is recognizable
    void sendFrame(uint32_t seq, uint8_t res, uint8_t zoom, uint16_t w, uint16_t h, uint8_t planes,
                   std::vector<uint16_t> pal, uint8_t first = 0) {
        std::vector<uint8_t> p(BLINK_FRAME_HEADER_SIZE);
        Utils::storeDword(&p[0], seq);
        p[4] = res;
        p[5] = zoom;
        Utils::storeWord(&p[6], w);
        Utils::storeWord(&p[8], h);
        p[10] = planes;
        Utils::storeWord(&p[12], pal.size());
        for(uint16_t c : pal) {
            p.push_back(c >> 8);
            p.push_back(c & 0xff);
        }
        size_t dataBytes = (size_t) (w / 16) * planes * 2 * h;
        for(size_t i=0; i<dataBytes; i++) {
            p.push_back((uint8_t) (first + i));
        }
        send(BLINK_FRAME, p);
    }
};

struct BrwHarness {
    BrwFakeDataTrans dt;
    BrowserModule   *bm;

    BrwHarness()  { bm = new BrowserModule(&dt); }
    ~BrwHarness() { delete bm; }

    uint8_t command(uint8_t c, std::vector<uint8_t> args = {}, std::vector<uint8_t> data = {}) {
        uint8_t cmd[16];
        memset(cmd, 0, sizeof(cmd));
        cmd[1] = 'C';
        cmd[2] = 'E';
        cmd[3] = HOSTMOD_BROWSER;
        cmd[4] = c;
        for(size_t i=0; i<args.size() && i<9; i++) {
            cmd[5 + i] = args[i];
        }
        dt.fromSt = data;
        dt.reply.clear();
        dt.lastStatus = 0xff;
        bm->processCommand(cmd);
        return dt.lastStatus;
    }

    uint8_t setMode(uint8_t res, uint16_t pageH, uint8_t zoom = 4) {
        return command(BRW_CMD_SET_MODE, { res, (uint8_t) (pageH >> 8), (uint8_t) pageH, zoom });
    }

    uint8_t getUpdate(uint32_t baseSeq, uint8_t maxSectors = 254, uint16_t resumeRow = 0) {
        return command(BRW_CMD_GET_UPDATE, { (uint8_t) (baseSeq >> 24), (uint8_t) (baseSeq >> 16), (uint8_t) (baseSeq >> 8),
                                             (uint8_t) baseSeq, maxSectors, (uint8_t) (resumeRow >> 8), (uint8_t) resumeRow });
    }

    uint8_t navigate(const std::string& url) {
        std::vector<uint8_t> d(BRW_URL_SIZE, 0);
        memcpy(d.data(), url.data(), url.size());
        return command(BRW_CMD_NAVIGATE, {}, d);
    }

    // IDENTIFY + connect the fake ce_browser + its HELLO
    void connect(FakeBrowser& fb) {
        REQUIRE(command(BRW_CMD_IDENTIFY) == BRW_OK);
        REQUIRE(bm->getLink().port() != 0);
        REQUIRE(fb.connectTo(bm->getLink().port()));
        std::vector<uint8_t> hello = fb.expect(BLINK_HELLO);
        REQUIRE(hello.size() == 2);
        CHECK(Utils::getWord(hello.data()) == BLINK_VERSION);
        REQUIRE(waitFor([&]() { return bm->getLink().isConnected(); }));
    }

    uint16_t word(size_t off) { return Utils::getWord(&dt.reply[off]); }
    uint32_t dword(size_t off) { return Utils::getDword(&dt.reply[off]); }
};

} // namespace

TEST_CASE("browser: identify and status without ce_browser")
{
    BrwHarness h;

    CHECK(h.command(BRW_CMD_IDENTIFY) == BRW_OK);
    REQUIRE(h.dt.reply.size() == 48);
    CHECK(std::string((const char *) h.dt.reply.data()) == "CosmosEx browser module");
    CHECK(h.word(32) == BROWSER_MODULE_VERSION);
    CHECK(h.dt.reply[34] == 0);                                     // not connected

    unsetenv("BROWSER_BIN");                                        // not configured - the status says so
    REQUIRE(waitFor([&]() { return h.bm->getLink().status().message.find("BROWSER_BIN") != std::string::npos; }));
    CHECK(h.command(BRW_CMD_GET_STATUS) == BRW_OK);
    REQUIRE(h.dt.reply.size() == BRW_STATUS_SIZE);
    CHECK((h.dt.reply[0] & BRW_STF_CONNECTED) == 0);
    CHECK(std::string((const char *) &h.dt.reply[BRW_STATUS_MSG_OFFSET]) == "ce_browser not configured - BROWSER_BIN in .env");

    CHECK(h.getUpdate(0) == BRW_ERR_NO_MODE);                       // no SET_MODE yet
    CHECK(h.setMode(SCREENCAST_RES_ST_HIGH, 384) == BRW_OK);        // kept for when ce_browser connects
    CHECK(h.getUpdate(0) == BRW_ERR_NOT_RUNNING);
    CHECK(h.navigate("https://example.com/") == BRW_OK);            // kept too
    CHECK(h.command(BRW_CMD_NAV_ACTION, { BRW_NAV_BACK }) == BRW_ERR_NOT_RUNNING);
    CHECK(h.command(BRW_CMD_KEY, { 0x1c, 13, 0 }) == BRW_ERR_NOT_RUNNING);
}

TEST_CASE("browser: bad parameters")
{
    BrwHarness h;

    CHECK(h.setMode(0x20, 200) == BRW_ERR_BAD_PARAM);               // Falcon resolution - not supported
    CHECK(h.setMode(SCREENCAST_RES_ST_HIGH, 401) == BRW_ERR_BAD_PARAM);     // taller than the screen
    CHECK(h.setMode(SCREENCAST_RES_ST_HIGH, 8) == BRW_ERR_BAD_PARAM);
    CHECK(h.setMode(SCREENCAST_RES_ST_HIGH, 384, 1) == BRW_ERR_BAD_PARAM);  // zoom 1/4
    CHECK(h.navigate("") == BRW_ERR_BAD_PARAM);
    CHECK(h.command(BRW_CMD_NAV_ACTION, { BRW_NAV_LAST + 1 }) == BRW_ERR_BAD_PARAM);
    CHECK(h.command(BRW_CMD_MOUSE, { BRW_MOUSE_LAST + 1 }) == BRW_ERR_BAD_PARAM);
    CHECK(h.command(0x7f) == BRW_ERR_BAD_PARAM);
}

TEST_CASE("browser: mode and URL set before ce_browser connects are sent when it does")
{
    BrwHarness h;
    FakeBrowser fb;

    CHECK(h.setMode(SCREENCAST_RES_TT_MID, 464, 8) == BRW_OK);
    CHECK(h.navigate("https://example.com/") == BRW_OK);
    h.connect(fb);

    std::vector<uint8_t> m = fb.expect(BLINK_SET_MODE);
    REQUIRE(m.size() == 7);
    CHECK(m[0] == SCREENCAST_RES_TT_MID);
    CHECK(m[1] == 8);
    CHECK(Utils::getWord(&m[2]) == 640);
    CHECK(Utils::getWord(&m[4]) == 464);
    CHECK(m[6] == 4);

    std::vector<uint8_t> u = fb.expect(BLINK_NAVIGATE);
    CHECK(std::string(u.begin(), u.end()) == "https://example.com/");

    CHECK(h.command(BRW_CMD_IDENTIFY) == BRW_OK);
    CHECK(h.dt.reply[34] == BRW_IDF_CONNECTED);
}

TEST_CASE("browser: commands are forwarded to ce_browser")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);

    CHECK(h.setMode(SCREENCAST_RES_ST_HIGH, 384) == BRW_OK);
    std::vector<uint8_t> m = fb.expect(BLINK_SET_MODE);
    CHECK(m[0] == SCREENCAST_RES_ST_HIGH);
    CHECK(m[1] == 4);                                               // zoom 0 from the ST = 1x
    CHECK(Utils::getWord(&m[2]) == 640);
    CHECK(Utils::getWord(&m[4]) == 384);
    CHECK(m[6] == 1);

    CHECK(h.navigate("atari st") == BRW_OK);
    std::vector<uint8_t> u = fb.expect(BLINK_NAVIGATE);
    CHECK(std::string(u.begin(), u.end()) == "atari st");

    CHECK(h.command(BRW_CMD_NAV_ACTION, { BRW_NAV_RELOAD }) == BRW_OK);
    CHECK(fb.expect(BLINK_NAV_ACTION) == std::vector<uint8_t>{ BRW_NAV_RELOAD });

    CHECK(h.command(BRW_CMD_MOUSE, { BRW_MOUSE_DOWN, 0x01, 0x23, 0x00, 0x45, 1 }) == BRW_OK);
    CHECK(fb.expect(BLINK_MOUSE) == std::vector<uint8_t>{ BRW_MOUSE_DOWN, 0x01, 0x23, 0x00, 0x45, 1 });

    CHECK(h.command(BRW_CMD_KEY, { 0x1e, 'a', 0x02 }) == BRW_OK);
    CHECK(fb.expect(BLINK_KEY) == std::vector<uint8_t>{ 0x1e, 'a', 0x02 });

    CHECK(h.command(BRW_CMD_SCROLL, { (uint8_t) -3, 0, 2, (uint8_t) -1 }) == BRW_OK);
    CHECK(fb.expect(BLINK_SCROLL) == std::vector<uint8_t>{ 0xfd, 0, 2, 0xff });
}

TEST_CASE("browser: full frame in one reply, then no change")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);
    REQUIRE(h.setMode(SCREENCAST_RES_ST_HIGH, 384) == BRW_OK);
    fb.expect(BLINK_SET_MODE);

    CHECK(h.getUpdate(0) == BRW_OK);                                // nothing rendered yet
    REQUIRE(h.dt.reply.size() == BRW_UPD_HEADER_SIZE);
    CHECK(h.dt.reply[4] == BRW_UPD_NO_CHANGE);

    fb.sendFrame(7, SCREENCAST_RES_ST_HIGH, 4, 640, 384, 1, { 0x0777, 0x0000 }, 3);
    REQUIRE(waitFor([&]() { return h.bm->getLink().latestFrame() != nullptr; }));

    CHECK(h.getUpdate(0) == BRW_OK);
    uint32_t rowBytes = 80;
    size_t   pixOff   = BRW_UPD_HEADER_SIZE + 4 + BRW_UPD_RECT_HEADER_SIZE;
    REQUIRE(h.dt.reply.size() >= pixOff + 384 * rowBytes);
    CHECK(h.dword(0) == 7);
    CHECK(h.dt.reply[4] == (BRW_UPD_FULL | BRW_UPD_PALETTE));
    CHECK(h.dt.reply[5] == SCREENCAST_RES_ST_HIGH);
    CHECK(h.word(6) == 1);
    CHECK(h.word(8) == 2);
    CHECK(h.word(10) == 640);
    CHECK(h.word(12) == 384);
    CHECK(h.dt.reply[14] == 1);
    CHECK(h.dt.reply[15] == 4);
    CHECK(h.word(16) == 0x0777);
    CHECK(h.word(18) == 0x0000);
    CHECK(h.word(20) == 0);                                         // rect x
    CHECK(h.word(22) == 0);                                         // rect y
    CHECK(h.word(24) == 40);                                        // rect width / 16
    CHECK(h.word(26) == 384);                                       // rect height
    bool same = true;
    for(size_t i=0; i<384 * rowBytes; i++) {
        same = same && h.dt.reply[pixOff + i] == (uint8_t) (3 + i);
    }
    CHECK(same);
    CHECK(h.dt.reply.size() % 16 == 0);

    CHECK(h.getUpdate(7) == BRW_OK);                                // the ST has frame 7 now
    REQUIRE(h.dt.reply.size() == BRW_UPD_HEADER_SIZE);
    CHECK(h.dt.reply[4] == BRW_UPD_NO_CHANGE);

    fb.sendFrame(8, SCREENCAST_RES_ST_HIGH, 4, 640, 384, 1, { 0x0777, 0x0000 });
    REQUIRE(waitFor([&]() { auto f = h.bm->getLink().latestFrame(); return f && f->seq == 8; }));
    CHECK(h.getUpdate(7) == BRW_OK);
    CHECK(h.dword(0) == 8);
    CHECK((h.dt.reply[4] & BRW_UPD_MORE) == 0);
}

TEST_CASE("browser: TT frame comes in parts")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);
    REQUIRE(h.setMode(SCREENCAST_RES_TT_HIGH, 944) == BRW_OK);
    fb.expect(BLINK_SET_MODE);

    fb.sendFrame(1, SCREENCAST_RES_TT_HIGH, 4, 1280, 944, 1, { 0x0fff, 0x0000 }, 0);
    REQUIRE(waitFor([&]() { return h.bm->getLink().latestFrame() != nullptr; }));

    const uint32_t rowBytes = 160;
    std::vector<uint8_t> got;
    uint16_t row = 0;
    int parts = 0;
    for(;;) {
        REQUIRE(h.getUpdate(0, 254, row) == BRW_OK);
        REQUIRE(parts < 10);
        parts++;
        CHECK(h.dword(0) == 1);
        bool first = (row == 0);
        uint16_t palCount = h.word(8);
        CHECK(palCount == (first ? 2 : 0));
        CHECK(((h.dt.reply[4] & BRW_UPD_PALETTE) != 0) == first);
        size_t rect = BRW_UPD_HEADER_SIZE + palCount * 2;
        CHECK(h.word(rect + 2) == row);
        uint16_t rows = h.word(rect + 6);
        REQUIRE(rows > 0);
        CHECK(h.dt.reply.size() <= 254 * 512);
        size_t pix = rect + BRW_UPD_RECT_HEADER_SIZE;
        got.insert(got.end(), h.dt.reply.begin() + pix, h.dt.reply.begin() + pix + rows * rowBytes);
        row += rows;
        if(!(h.dt.reply[4] & BRW_UPD_MORE)) {
            break;
        }
    }
    CHECK(parts == 2);
    CHECK(row == 944);
    REQUIRE(got.size() == 944 * rowBytes);
    bool same = true;
    for(size_t i=0; i<got.size(); i++) {
        same = same && got[i] == (uint8_t) i;
    }
    CHECK(same);

    // a new frame arriving between the parts doesn't mix into the one being sent
    REQUIRE(h.getUpdate(1, 254, 0) == BRW_OK);                      // still frame 1 is newest - no change
    CHECK(h.dt.reply[4] == BRW_UPD_NO_CHANGE);
    fb.sendFrame(2, SCREENCAST_RES_TT_HIGH, 4, 1280, 944, 1, { 0x0fff, 0x0000 }, 100);
    REQUIRE(waitFor([&]() { auto f = h.bm->getLink().latestFrame(); return f && f->seq == 2; }));
    REQUIRE(h.getUpdate(1, 254, 0) == BRW_OK);
    CHECK(h.dword(0) == 2);
    uint16_t next = h.word(BRW_UPD_HEADER_SIZE + 4 + 6);
    fb.sendFrame(3, SCREENCAST_RES_TT_HIGH, 4, 1280, 944, 1, { 0x0fff, 0x0000 }, 200);
    REQUIRE(waitFor([&]() { auto f = h.bm->getLink().latestFrame(); return f && f->seq == 3; }));
    REQUIRE(h.getUpdate(1, 254, next) == BRW_OK);
    CHECK(h.dword(0) == 2);                                         // rest of frame 2, not 3
    CHECK(h.dt.reply[BRW_UPD_HEADER_SIZE + BRW_UPD_RECT_HEADER_SIZE] == (uint8_t) (100 + next * rowBytes));
}

TEST_CASE("browser: too few sectors for one row")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);
    REQUIRE(h.setMode(SCREENCAST_RES_TT_LOW, 464) == BRW_OK);
    fb.expect(BLINK_SET_MODE);

    std::vector<uint16_t> pal(256, 0x0123);
    fb.sendFrame(1, SCREENCAST_RES_TT_LOW, 4, 320, 464, 8, pal);
    REQUIRE(waitFor([&]() { return h.bm->getLink().latestFrame() != nullptr; }));

    CHECK(h.getUpdate(0, 1) == BRW_ERR_BAD_PARAM);                  // 512 B palette alone fills a sector
    CHECK(h.getUpdate(0, 2) == BRW_OK);
    CHECK(h.word(8) == 256);
    CHECK(h.word(BRW_UPD_HEADER_SIZE + 512 + 6) == 1);              // 1 row of 320 B fits
}

TEST_CASE("browser: frames of another mode or broken frames are dropped")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);
    REQUIRE(h.setMode(SCREENCAST_RES_ST_HIGH, 384) == BRW_OK);
    fb.expect(BLINK_SET_MODE);

    fb.sendFrame(1, SCREENCAST_RES_ST_HIGH, 8, 640, 384, 1, { 0x0777, 0 });        // other zoom
    fb.sendFrame(2, SCREENCAST_RES_ST_HIGH, 4, 640, 400, 1, { 0x0777, 0 });        // other height
    fb.sendFrame(3, SCREENCAST_RES_ST_MID,  4, 640, 384, 2, { 0x0777, 0, 0, 0 });  // other resolution
    std::vector<uint8_t> broken(BLINK_FRAME_HEADER_SIZE + 10, 0);                  // sizes don't add up
    Utils::storeDword(&broken[0], 4);
    broken[4] = SCREENCAST_RES_ST_HIGH; broken[5] = 4;
    Utils::storeWord(&broken[6], 640); Utils::storeWord(&broken[8], 384); broken[10] = 1;
    fb.send(BLINK_FRAME, broken);
    fb.sendFrame(5, SCREENCAST_RES_ST_HIGH, 4, 640, 384, 1, { 0x0777, 0 });        // this one is right

    REQUIRE(waitFor([&]() { return h.bm->getLink().latestFrame() != nullptr; }));
    CHECK(h.bm->getLink().latestFrame()->seq == 5);

    REQUIRE(h.setMode(SCREENCAST_RES_ST_HIGH, 368) == BRW_OK);      // new mode - old frames gone
    CHECK(!h.bm->getLink().latestFrame());
    CHECK(h.getUpdate(5) == BRW_OK);
    CHECK(h.dt.reply[4] == BRW_UPD_NO_CHANGE);
}

TEST_CASE("browser: status from ce_browser, disconnect")
{
    BrwHarness h;
    FakeBrowser fb;
    h.connect(fb);

    std::vector<uint8_t> s = { BRW_STF_LOADING | BRW_STF_CAN_BACK, 42, 0x00, 0x10, 0x00, 0x20 };
    auto addStr = [&](const std::string& str) {
        s.push_back(str.size() >> 8);
        s.push_back(str.size() & 0xff);
        s.insert(s.end(), str.begin(), str.end());
    };
    addStr("https://example.com/");
    addStr("Example Domain");
    addStr("");
    fb.send(BLINK_STATUS, s);
    REQUIRE(waitFor([&]() { return h.bm->getLink().status().progress == 42; }));

    CHECK(h.command(BRW_CMD_GET_STATUS) == BRW_OK);
    CHECK(h.dt.reply[0] == (BRW_STF_CONNECTED | BRW_STF_LOADING | BRW_STF_CAN_BACK));
    CHECK(h.dt.reply[1] == 42);
    CHECK(h.word(2) == 0x10);
    CHECK(h.word(4) == 0x20);
    CHECK(std::string((const char *) &h.dt.reply[BRW_STATUS_URL_OFFSET]) == "https://example.com/");
    CHECK(std::string((const char *) &h.dt.reply[BRW_STATUS_TITLE_OFFSET]) == "Example Domain");

    close(fb.fd);                                                   // ce_browser gone
    fb.fd = -1;
    REQUIRE(waitFor([&]() { return !h.bm->getLink().isConnected(); }));
    CHECK(h.command(BRW_CMD_GET_STATUS) == BRW_OK);
    CHECK(h.dt.reply[0] == 0);                                      // no stale LOADING / CAN_BACK from the gone ce_browser
    CHECK(h.dt.reply[1] == 0);
    CHECK(std::string((const char *) &h.dt.reply[BRW_STATUS_MSG_OFFSET]).size() > 0);   // why it's not there
    CHECK(h.command(BRW_CMD_NAV_ACTION, { BRW_NAV_BACK }) == BRW_ERR_NOT_RUNNING);

    FakeBrowser fb2;                                                // restarted ce_browser gets the HELLO again
    REQUIRE(fb2.connectTo(h.bm->getLink().port()));
    fb2.expect(BLINK_HELLO);
    REQUIRE(waitFor([&]() { return h.bm->getLink().isConnected(); }));
}

namespace {

std::string tempHistoryFile(void)
{
    char path[] = "/tmp/ce_brw_histXXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(path);                                                   // start without the file
    BrowserHistory::setFilePath(path);
    return path;
}

// GET_HISTORY -> the URLs
std::vector<std::string> getHistory(BrwHarness& h, uint8_t maxCount, uint8_t maxSectors)
{
    REQUIRE(h.command(BRW_CMD_GET_HISTORY, { maxCount, maxSectors }) == BRW_OK);
    REQUIRE(h.dt.reply.size() >= 2);
    CHECK(h.dt.reply.size() % 16 == 0);
    CHECK(h.dt.reply.size() <= (size_t) maxSectors * 512);
    std::vector<std::string> out;
    size_t pos = 2;
    for(int i=0; i<h.word(0); i++) {
        std::string u((const char *) &h.dt.reply[pos]);
        out.push_back(u);
        pos += u.size() + 1;
    }
    return out;
}

} // namespace

TEST_CASE("browser: URL history")
{
    std::string file = tempHistoryFile();
    BrwHarness h;

    CHECK(getHistory(h, 0, 1).empty());                             // works without ce_browser, empty at first

    FakeBrowser fb;
    h.connect(fb);
    auto add = [&](const std::string& u) { fb.send(BLINK_HISTORY_ADD, std::vector<uint8_t>(u.begin(), u.end())); };
    add("https://a.example/");
    add("https://b.example/");
    add("https://a.example/");                                      // again - moves to the top, not twice
    REQUIRE(waitFor([&]() { return BrowserHistory::list(0).size() == 2 && BrowserHistory::list(0)[0] == "https://a.example/"; }));

    CHECK(getHistory(h, 0, 1) == std::vector<std::string>{ "https://a.example/", "https://b.example/" });
    CHECK(getHistory(h, 1, 1) == std::vector<std::string>{ "https://a.example/" });

    BrowserHistory::setFilePath(file);                              // reload - it was saved
    CHECK(BrowserHistory::list(0) == std::vector<std::string>{ "https://a.example/", "https://b.example/" });

    for(int i=0; i<BRW_HISTORY_MAX + 5; i++) {                      // only the newest BRW_HISTORY_MAX stay
        BrowserHistory::add("https://site" + std::to_string(i) + ".example/" + std::string(100, 'x'));
    }
    std::vector<std::string> all = BrowserHistory::list(0);
    REQUIRE(all.size() == BRW_HISTORY_MAX);
    CHECK(all[0].find("site104.") != std::string::npos);

    std::vector<std::string> some = getHistory(h, 0, 1);            // only as many as fit in one sector
    CHECK(some.size() == 4);                                        // 2 + 4 x ~125 bytes <= 512
    CHECK(getHistory(h, 0, 254).size() == BRW_HISTORY_MAX);

    BrowserHistory::add("bad\nurl");
    BrowserHistory::add("");
    CHECK(BrowserHistory::list(0).size() == BRW_HISTORY_MAX);

    unlink(file.c_str());
}

//--------------------------------------------------------
// With the real ce_browser - only when CE_BROWSER_TEST_BIN is set (needs Chrome and network), e.g.:
//   CE_BROWSER_TEST_BIN=../ce_browser/ce_browser CE_BROWSER_TEST_OUT=/tmp ./tests/run_tests.elf -tc="*real ce_browser*"
// CE_BROWSER_TEST_OUT: where to write the received frames as .ppm images

namespace {

// whole frame through GET_UPDATE (all parts) -> RGB pixels; returns seq, 0 if none
uint32_t fetchFrame(BrwHarness& h, uint32_t baseSeq, int& w, int& hgt, std::vector<uint8_t>& rgb)
{
    std::vector<uint16_t> pal;
    uint16_t row = 0;
    uint32_t seq = 0;
    int planes = 1;
    for(int part = 0; part < 10; part++) {
        REQUIRE(h.getUpdate(baseSeq, 254, row) == BRW_OK);
        uint8_t flags = h.dt.reply[4];
        if(flags & BRW_UPD_NO_CHANGE) {
            return 0;
        }
        seq    = h.dword(0);
        w      = h.word(10);
        hgt    = h.word(12);
        planes = h.dt.reply[14];
        uint16_t palCount = h.word(8);
        if(row == 0) {
            rgb.assign((size_t) w * hgt * 3, 0);
            pal.clear();
            for(int i=0; i<palCount; i++) {
                pal.push_back(h.word(BRW_UPD_HEADER_SIZE + i * 2));
            }
        }
        size_t rect = BRW_UPD_HEADER_SIZE + palCount * 2;
        uint16_t y0 = h.word(rect + 2), rows = h.word(rect + 6), groups = h.word(rect + 4);
        const uint8_t *d = &h.dt.reply[rect + BRW_UPD_RECT_HEADER_SIZE];
        int maxLevel = (planes == 1) ? 1 : ((h.dt.reply[5] & 0x10) ? 15 : 7);    // TT: 4 bits per channel, ST: 3
        for(int y = 0; y < rows; y++) {
            for(int g = 0; g < groups; g++) {
                for(int bit = 0; bit < 16; bit++) {
                    int idx = 0;
                    for(int p = 0; p < planes; p++) {
                        uint16_t word = (d[((y * groups + g) * planes + p) * 2] << 8) | d[((y * groups + g) * planes + p) * 2 + 1];
                        idx |= ((word >> (15 - bit)) & 1) << p;
                    }
                    uint8_t *px = &rgb[(((size_t) (y0 + y)) * w + g * 16 + bit) * 3];
                    if(planes == 1) {
                        px[0] = px[1] = px[2] = idx ? 0 : 255;
                    } else {
                        uint16_t c = idx < (int) pal.size() ? pal[idx] : 0;
                        px[0] = ((c >> 8) & 0xf) * 255 / maxLevel;
                        px[1] = ((c >> 4) & 0xf) * 255 / maxLevel;
                        px[2] = (c & 0xf) * 255 / maxLevel;
                    }
                }
            }
        }
        row = y0 + rows;
        if(!(flags & BRW_UPD_MORE)) {
            break;
        }
    }
    CHECK(row == hgt);
    return seq;
}

void savePpm(const char *name, int w, int h, const std::vector<uint8_t>& rgb)
{
    const char *dir = getenv("CE_BROWSER_TEST_OUT");
    if(!dir) {
        return;
    }
    std::string path = std::string(dir) + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    if(f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(rgb.data(), 1, rgb.size(), f);
        fclose(f);
        MESSAGE("frame saved to " << path);
    }
}

std::vector<std::string> seenMessages;                              // status messages seen by waitForPage(), in order

// waits for a frame newer than baseSeq whose page shows a finished load of 'title'
uint32_t waitForPage(BrwHarness& h, const std::string& title, uint32_t baseSeq, int& w, int& hgt, std::vector<uint8_t>& rgb)
{
    seenMessages.clear();
    REQUIRE(waitFor([&]() {
        BrowserStatus s = h.bm->getLink().status();
        if(seenMessages.empty() || seenMessages.back() != s.message) {
            seenMessages.push_back(s.message);
        }
        return s.title == title && !(s.flags & BRW_STF_LOADING) && s.message.empty();
    }, 30000));
    Utils::sleepMs(2000);                                           // let the last frames of the load come
    uint32_t seq = 0;
    REQUIRE(waitFor([&]() { seq = fetchFrame(h, baseSeq, w, hgt, rgb); return seq != 0; }, 10000));
    return seq;
}

} // namespace

TEST_CASE("browser: real ce_browser" * doctest::skip(getenv("CE_BROWSER_TEST_BIN") == NULL))
{
    setenv("BROWSER_BIN", getenv("CE_BROWSER_TEST_BIN"), 1);
    std::string histFile = tempHistoryFile();
    BrwHarness h;

    REQUIRE(h.command(BRW_CMD_IDENTIFY) == BRW_OK);                 // starts ce_browser
    REQUIRE(waitFor([&]() { return h.bm->getLink().isConnected(); }, 15000));

    // ST high, page area under a 16 line toolbar
    REQUIRE(h.setMode(SCREENCAST_RES_ST_HIGH, 384) == BRW_OK);
    REQUIRE(h.navigate("https://example.com/") == BRW_OK);

    int w = 0, hgt = 0;
    std::vector<uint8_t> rgb;
    uint32_t seq = waitForPage(h, "Example Domain", 0, w, hgt, rgb);
    std::string msgs;
    for(const std::string& m : seenMessages) {
        msgs += "[" + m + "] ";
    }
    MESSAGE("status messages: " << msgs);
    bool sawProgress = false;                                       // ce_browser said what it was doing on the way
    for(const std::string& m : seenMessages) {
        sawProgress = sawProgress || m.find("Opening example.com") == 0 || m == "Rendering page..." || m == "Starting ce_browser...";
    }
    CHECK(sawProgress);
    CHECK(w == 640);
    CHECK(hgt == 384);
    size_t black = 0;
    for(size_t i=0; i<rgb.size(); i += 3) {
        black += (rgb[i] == 0);
    }
    CHECK(black > 500);                                             // there is text
    CHECK(black < (size_t) (w * hgt) / 2);                          // but mostly white page
    savePpm("brw_st_high.ppm", w, hgt, rgb);

    CHECK(h.command(BRW_CMD_GET_STATUS) == BRW_OK);
    CHECK((h.dt.reply[0] & BRW_STF_CONNECTED) != 0);
    CHECK(std::string((const char *) &h.dt.reply[BRW_STATUS_URL_OFFSET]) == "https://example.com/");

    // TT mid, zoom 2 - comes in two parts
    REQUIRE(h.setMode(SCREENCAST_RES_TT_MID, 464, 8) == BRW_OK);
    std::vector<uint8_t> rgb2;
    REQUIRE(waitFor([&]() { seq = fetchFrame(h, seq, w, hgt, rgb2); return seq != 0; }, 15000));
    CHECK(w == 640);
    CHECK(hgt == 464);
    savePpm("brw_tt_mid_z2.ppm", w, hgt, rgb2);

    // another page, and back
    REQUIRE(h.navigate("https://en.wikipedia.org/wiki/Atari_TT030") == BRW_OK);
    seq = waitForPage(h, "Atari TT030 - Wikipedia", seq, w, hgt, rgb2);
    savePpm("brw_tt_mid_z2_wiki.ppm", w, hgt, rgb2);
    CHECK(h.command(BRW_CMD_GET_STATUS) == BRW_OK);
    CHECK((h.dt.reply[0] & BRW_STF_CAN_BACK) != 0);

    REQUIRE(h.command(BRW_CMD_NAV_ACTION, { BRW_NAV_BACK }) == BRW_OK);
    waitForPage(h, "Example Domain", seq, w, hgt, rgb2);

    // scrolling as with the mouse at the edge: SCROLL, then wait for its frame - how fast do frames come?
    auto scrollRate = [&](uint8_t res, uint16_t pageH, int8_t lines, const char *name) {
        REQUIRE(h.setMode(res, pageH, 4) == BRW_OK);
        REQUIRE(h.navigate("https://en.wikipedia.org/wiki/Atari_ST") == BRW_OK);
        seq = waitForPage(h, "Atari ST - Wikipedia", 0, w, hgt, rgb2);
        Utils::sleepMs(1000);
        seq = fetchFrame(h, seq, w, hgt, rgb2) ?: seq;

        std::vector<uint32_t> ms;
        for(int i=0; i<15; i++) {
            uint32_t t0 = Utils::getCurrentMs();
            REQUIRE(h.command(BRW_CMD_SCROLL, { (uint8_t) lines, 0, 0, 0 }) == BRW_OK);
            uint32_t got = 0;
            REQUIRE(waitFor([&]() { got = fetchFrame(h, seq, w, hgt, rgb2); return got != 0; }, 5000));
            seq = got;
            ms.push_back(Utils::elapsedMs(t0));
        }
        uint32_t sum = 0, mx = 0;
        for(size_t i=1; i<ms.size(); i++) {                         // the 1st one captures the strip
            sum += ms[i];
            mx = MAX(mx, ms[i]);
        }
        uint32_t avg = sum / (ms.size() - 1);
        MESSAGE(std::string(name) << ": first step " << ms[0] << " ms, then avg " << avg << " ms, max " << mx << " ms per scroll frame");
        return avg;
    };
    CHECK(scrollRate(SCREENCAST_RES_ST_HIGH, 382, 2, "ST high") < 100);    // 10 fps
    CHECK(scrollRate(SCREENCAST_RES_TT_MID, 462, 4, "TT mid") < 200);      // 5 fps
    savePpm("brw_tt_mid_scrolled.ppm", w, hgt, rgb2);

    std::vector<std::string> hist = getHistory(h, 10, 2);           // both pages loaded fine - in the history, newest first
    REQUIRE(hist.size() >= 2);
    CHECK(hist[0] == "https://en.wikipedia.org/wiki/Atari_ST");
    CHECK(hist[1] == "https://example.com/");
    unlink(histFile.c_str());
}
