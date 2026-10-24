// image_browser.cpp - browsing the images of the current folder on the front panel: the list comes from the host in pages
// (asked for only with the panel attached), left / right move through it and wrap around, enter inserts the shown image
// by its name, the cursor stays on the same image when the list changes, 15 s without a key ends the browsing.

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_connection.h"
#include "fake_panel_uart.h"

#include "defs.h"
#include "utils.h"
#include "panel.h"
#include "host_receive.h"
#include "image_browser.h"

extern int imageState;

namespace {

std::vector<uint8_t> message(uint16_t cmd, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> m(TX_HEADER_SIZE);
    storeHeader(m.data(), cmd, data.size());
    m.insert(m.end(), data.begin(), data.end());
    return m;
}

std::vector<uint8_t> pageData(uint32_t gen, uint32_t total, uint32_t current, uint32_t start, const std::vector<std::string>& names)
{
    std::vector<uint8_t> d(IMAGE_LIST_PAGE_HEADER);
    storeDword(d.data(), gen);
    storeDword(d.data() + 4, total);
    storeDword(d.data() + 8, current);
    storeDword(d.data() + 12, start);
    storeWord(d.data() + 16, names.size());
    for (const std::string& n : names) {
        d.push_back(n.size());
        d.insert(d.end(), n.begin(), n.end());
    }
    return d;
}

// names "g<index>.st" for indexes first..first+count-1
std::vector<std::string> names(uint32_t first, uint32_t count)
{
    std::vector<std::string> v;
    for (uint32_t i = first; i < first + count; i++) {
        v.push_back("g" + std::to_string(i) + ".st");
    }
    return v;
}

void hostSendsPage(uint32_t gen, uint32_t total, uint32_t current, uint32_t start, const std::vector<std::string>& n)
{
    fakeHostReceive(message(ATN_GET_IMAGE_LIST, pageData(gen, total, current, start, n)));
    handleIncommingData();
}

void hostSendsInsertStatus(uint8_t status)
{
    fakeHostReceive(message(ATN_INSERT_IMAGE, { status }));
    handleIncommingData();
}

struct Sent {
    uint16_t cmd;
    std::vector<uint8_t> data;
};

std::vector<Sent> takeSent(void)
{
    std::vector<Sent> v;
    for (const std::vector<uint8_t>& m : fakeHostSent) {
        REQUIRE(m.size() >= TX_HEADER_SIZE);
        Sent s = { getWord((uint8_t*) m.data() + 4), std::vector<uint8_t>(m.begin() + TX_HEADER_SIZE, m.end()) };
        CHECK(getDword((uint8_t*) m.data() + 6) == s.data.size());
        v.push_back(s);
    }
    fakeHostSent.clear();
    return v;
}

// the only message sent must be a page request - returns its start
uint32_t takePageRequest(void)
{
    std::vector<Sent> s = takeSent();
    REQUIRE(s.size() == 1);
    CHECK(s[0].cmd == ATN_GET_IMAGE_LIST);
    REQUIRE(s[0].data.size() == 6);
    CHECK(getWord(s[0].data.data() + 4) == IMAGE_LIST_PAGE_MAX);
    return getDword(s[0].data.data());
}

std::string panelTx;    // everything sent to the panel since the fixture started

void loop(int times = 1)
{
    for (int i = 0; i < times; i++) {
        browserLoop();
        for (int j = 0; j < 200; j++) {     // the panel UART takes 8 bytes per call
            panelLoop();
        }
        panelTx += fakePanelTx;
        fakePanelTx.clear();
    }
}

// text of the last T message for this line
std::string panelLine(int line)
{
    std::string tag = "T" + std::to_string(line);
    size_t pos = std::string::npos, p = 0;
    while ((p = panelTx.find(tag, p)) != std::string::npos) {
        if (p == 0 || panelTx[p - 1] == '\n') {
            pos = p;
        }
        p++;
    }
    if (pos == std::string::npos) {
        return "<none>";
    }
    size_t colon = panelTx.find(':', pos);
    size_t end = panelTx.find('\n', colon);
    return panelTx.substr(colon + 1, end - colon - 1);
}

struct BrowserFixture {
    BrowserFixture(const char* identify = "I1,4,25,18,SD,0.1\n") {
        fakeReset();
        fakeHostReset();
        fakePanelReset();
        panelTx.clear();
        panelInit();
        if (identify) {
            fakePanelReceive(identify);
        }
        loop();
        imageState = IMAGE_LOADED;
        browserReset();
    }

    ~BrowserFixture() {
        imageState = IMAGE_NOT_LOADED;
        browserReset();
    }

    // the list asked for and received: 'total' images, page of 16 around 'current'
    void haveList(uint32_t total, uint32_t current, uint32_t gen = 1) {
        loop();
        CHECK(takePageRequest() == IMAGE_LIST_NONE);
        uint32_t start = (current >= total || current < 8) ? 0 : current - 8;
        if (total > 16 && start > total - 16) {
            start = total - 16;
        }
        uint32_t count = (total < 16) ? total : 16;
        hostSendsPage(gen, total, current, start, names(start, count));
    }
};

struct SmallPanelFixture : BrowserFixture {
    SmallPanelFixture() : BrowserFixture("I1,1,12,6,SD,0.1\n") {}
};

struct NoPanelFixture : BrowserFixture {
    NoPanelFixture() : BrowserFixture(NULL) {}
};

}   // namespace

TEST_CASE_FIXTURE(NoPanelFixture, "image browser: without the panel the list isn't asked for")
{
    loop(5);
    CHECK(takeSent().empty());
    CHECK_FALSE(browserKey('R', 'S'));
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: list asked for once the image is loaded, again when there's no reply")
{
    imageState = IMAGE_REQUESTED;
    loop();
    CHECK(takeSent().empty());              // image still loading

    imageState = IMAGE_LOADED;
    loop();
    CHECK(takePageRequest() == IMAGE_LIST_NONE);

    loop(3);
    CHECK(takeSent().empty());              // waiting for the reply

    fakeNowMs += BROWSE_REQUEST_RETRY_MS;
    loop();
    CHECK(takePageRequest() == IMAGE_LIST_NONE);

    hostSendsPage(1, 3, 1, 0, names(0, 3));
    fakeNowMs += 5000;
    loop(3);
    CHECK(takeSent().empty());              // got it - the host sends the changes on its own
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: keys are not used before the list is here, or when it's empty")
{
    CHECK_FALSE(browserKey('R', 'S'));
    CHECK_FALSE(browserKey('E', 'S'));

    haveList(0, IMAGE_LIST_NONE);
    CHECK_FALSE(browserKey('R', 'S'));
    CHECK_FALSE(browserKey('L', 'S'));
    CHECK_FALSE(browserActive());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: left / right move from the current image and wrap around")
{
    haveList(3, 1);
    CHECK_FALSE(browserActive());

    CHECK(browserKey('R', 'S'));
    CHECK(browserActive());
    loop();
    CHECK(panelLine(0) == "g2.st");
    CHECK(panelLine(2) == "< 3/3 >");
    CHECK(panelLine(3) == "enter: insert");

    CHECK(browserKey('R', 'S'));            // last -> first
    loop();
    CHECK(panelLine(0) == "g0.st");
    CHECK(panelLine(2) == "< 1/3 >");

    CHECK(browserKey('L', 'S'));            // first -> last
    loop();
    CHECK(panelLine(0) == "g2.st");

    CHECK(browserKey('L', 'S'));            // back on the current one
    loop();
    CHECK(panelLine(0) == "g1.st");
    CHECK(panelLine(3) == "in drive");

    CHECK_FALSE(browserKey('L', 'H'));      // long presses are not used
    CHECK_FALSE(browserKey('R', 'H'));
    CHECK(takeSent().empty());              // whole list in the page - nothing to ask for
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: current image not in the list - right starts at the first, left at the last")
{
    haveList(5, IMAGE_LIST_NONE);
    CHECK(browserKey('R', 'S'));
    loop();
    CHECK(panelLine(0) == "g0.st");

    browserReset();
    haveList(5, IMAGE_LIST_NONE);
    CHECK(browserKey('L', 'S'));
    loop();
    CHECK(panelLine(0) == "g4.st");
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: next page asked for near the page edge, name shown when it comes")
{
    haveList(3720, 100);                    // page 92..107
    takeSent();

    for (int i = 0; i < 5; i++) {           // 101..105 - still not close to the end of the page
        browserKey('R', 'S');
        loop();
    }
    CHECK(takeSent().empty());
    CHECK(panelLine(0) == "g105.st");

    browserKey('R', 'S');                   // 106 - one before the page end
    loop();
    CHECK(takePageRequest() == 106 - 8);
    loop(3);
    CHECK(takeSent().empty());              // one request at a time

    browserKey('R', 'S');                   // 107
    browserKey('R', 'S');                   // 108 - not in the page
    loop();
    CHECK(panelLine(0) == "...");
    CHECK(panelLine(2) == "< 109/3720 >");
    CHECK(browserKey('E', 'S'));            // no name yet - nothing inserted
    CHECK(takeSent().empty());

    hostSendsPage(1, 3720, 100, 98, names(98, 16));     // the reply to the request for 98
    loop();
    CHECK(panelLine(0) == "g108.st");
    CHECK(takeSent().empty());              // 108 is well inside 98..113
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: wrapping to the end of a long list asks for the last page")
{
    haveList(3720, 0);
    takeSent();

    browserKey('L', 'S');                   // 0 -> 3719
    loop();
    CHECK(panelLine(2) == "< 3720/3720 >");
    CHECK(takePageRequest() == 3719 - 8);

    hostSendsPage(1, 3720, 0, 3704, names(3704, 16));   // host moved the page back to be full
    loop();
    CHECK(panelLine(0) == "g3719.st");
    CHECK(takeSent().empty());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: enter inserts the shown image by name, loading ends the browsing")
{
    haveList(10, 2, 7);
    browserKey('R', 'S');
    browserKey('R', 'S');                   // 4
    CHECK(browserKey('E', 'S'));

    std::vector<Sent> s = takeSent();
    REQUIRE(s.size() == 1);
    CHECK(s[0].cmd == ATN_INSERT_IMAGE);
    REQUIRE(s[0].data.size() == 9 + 5);
    CHECK(getDword(s[0].data.data()) == 7);         // gen
    CHECK(getDword(s[0].data.data() + 4) == 4);     // index
    CHECK(s[0].data[8] == 5);
    CHECK(std::string(s[0].data.begin() + 9, s[0].data.end()) == "g4.st");

    loop();
    CHECK(panelLine(3) == "loading...");
    CHECK_FALSE(browserKey('R', 'S'));      // waiting for the reply
    browserKey('E', 'S');
    CHECK(takeSent().empty());              // not sent twice

    hostSendsInsertStatus(INSERT_IMAGE_OK);
    CHECK_FALSE(browserActive());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: image not found is shown, browsing goes on")
{
    haveList(10, 2);
    browserKey('R', 'S');
    browserKey('E', 'S');
    takeSent();

    hostSendsInsertStatus(INSERT_IMAGE_NOT_FOUND);
    loop();
    CHECK(browserActive());
    CHECK(panelLine(3) == "not found");

    fakeNowMs += BROWSE_STATUS_MS;
    loop();
    CHECK(panelLine(3) == "enter: insert");

    CHECK(browserKey('R', 'S'));            // keys work again
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: no reply to the insert - can be tried again")
{
    haveList(10, 2);
    browserKey('R', 'S');
    browserKey('E', 'S');
    takeSent();

    fakeNowMs += BROWSE_INSERT_TIMEOUT_MS;
    loop();
    CHECK(panelLine(3) == "no reply");
    CHECK(browserKey('E', 'S'));
    CHECK(takeSent().size() == 1);
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: enter on the image in the drive just ends the browsing")
{
    haveList(10, 2);
    browserKey('R', 'S');
    browserKey('L', 'S');
    CHECK(browserKey('E', 'S'));
    CHECK_FALSE(browserActive());
    CHECK(takeSent().empty());
    CHECK_FALSE(browserKey('E', 'S'));      // not browsing - enter is free (long press = captive portal elsewhere)
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: 15 s without a key ends the browsing")
{
    haveList(10, 2);
    browserKey('R', 'S');

    fakeNowMs += BROWSE_TIMEOUT_MS - 1;
    loop();
    CHECK(browserActive());

    browserKey('R', 'S');                   // a key starts the time again
    fakeNowMs += BROWSE_TIMEOUT_MS - 1;
    loop();
    CHECK(browserActive());

    fakeNowMs += 1;
    loop();
    CHECK_FALSE(browserActive());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: list changed - cursor stays on the same image, or the same index")
{
    haveList(10, 2, 1);
    browserKey('R', 'S');
    browserKey('R', 'S');                   // g4.st at 4

    std::vector<std::string> n = names(0, 10);
    n.insert(n.begin(), "a_new.st");        // added before it
    hostSendsPage(2, 11, 3, 0, n);
    loop();
    CHECK(panelLine(0) == "g4.st");
    CHECK(panelLine(2) == "< 6/11 >");

    n.erase(n.begin() + 5);                 // g4.st deleted - the next one is at its index now
    hostSendsPage(3, 10, 3, 0, n);
    loop();
    CHECK(panelLine(0) == "g5.st");
    CHECK(panelLine(2) == "< 6/10 >");

    browserKey('L', 'S');
    browserKey('L', 'S');
    browserKey('L', 'S');
    browserKey('L', 'S');
    browserKey('L', 'S');
    browserKey('L', 'S');                   // 0 -> 9 (wrap)
    hostSendsPage(4, 3, 0, 0, names(0, 3)); // most deleted - cursor beyond the end goes to the last one
    loop();
    CHECK(panelLine(2) == "< 3/3 >");

    hostSendsPage(5, 0, IMAGE_LIST_NONE, 0, {});    // all deleted - nothing to browse
    loop();
    CHECK_FALSE(browserActive());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: other image loading (e.g. from the web UI) ends the browsing")
{
    haveList(10, 2);
    browserKey('R', 'S');
    imageState = IMAGE_REQUESTED;
    loop();
    CHECK_FALSE(browserActive());
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: lost connection forgets the list, it's asked for again")
{
    haveList(10, 2);
    browserKey('R', 'S');

    fakeHostConnected = false;
    handleIncommingData();
    CHECK_FALSE(browserActive());
    CHECK_FALSE(browserKey('R', 'S'));

    fakeHostConnected = true;
    loop();
    CHECK(takePageRequest() == IMAGE_LIST_NONE);
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: broken page - names up to the broken one, page beyond the list ignored")
{
    loop();
    takeSent();

    std::vector<uint8_t> d = pageData(1, 10, 0, 0, names(0, 3));
    storeWord(d.data() + 16, 5);            // says 5 names, has 3
    d.push_back(200);                       // and a 4th one cut off
    d.push_back('x');
    fakeHostReceive(message(ATN_GET_IMAGE_LIST, d));
    handleIncommingData();

    CHECK(browserKey('R', 'S'));
    loop();
    CHECK(panelLine(0) == "g1.st");
    CHECK(takePageRequest() == 0);          // close to the end of the 3 names, 10 in the list
    browserKey('R', 'S');
    loop();
    CHECK(panelLine(0) == "g2.st");
    browserKey('R', 'S');                   // 3 isn't here
    loop();
    CHECK(panelLine(0) == "...");

    hostSendsPage(1, 2, 0, 5, names(5, 3)); // page beyond the 2 images left - no names, cursor to the last one
    loop();
    CHECK(panelLine(0) == "...");
    CHECK(panelLine(2) == "< 2/2 >");
}

TEST_CASE_FIXTURE(BrowserFixture, "image browser: long name - longest possible name is kept whole for the insert")
{
    std::string longName = std::string(IMAGE_LIST_NAME_MAX - 3, 'x') + ".st";
    loop();
    takeSent();
    hostSendsPage(1, 2, 0, 0, { "a.st", longName });

    browserKey('R', 'S');
    browserKey('E', 'S');
    std::vector<Sent> s = takeSent();
    REQUIRE(s.size() == 1);
    CHECK(s[0].data[8] == IMAGE_LIST_NAME_MAX);
    CHECK(std::string(s[0].data.begin() + 9, s[0].data.end()) == longName);
}

TEST_CASE_FIXTURE(SmallPanelFixture, "image browser: one line panel shows the index before the name")
{
    haveList(10, 2);
    browserKey('R', 'S');
    loop();
    CHECK(panelLine(0) == "4 g3.st");
    CHECK(panelLine(1) == "<none>");        // the panel has just one line, the second isn't sent
}
