// panel.cpp - the FDD side of the front panel (ce_pico_display/protocol.md): only changed lines are sent, the whole
// screen again when the panel announces itself, the UART is never waited for (8 byte fifo), key events are passed on.

#include <string>

#include "doctest.h"
#include "fakes_fdd.h"
#include "fake_panel_uart.h"

#include "panel.h"

namespace {

// runs the main loop until everything queued is sent
std::string sendAll(void)
{
    fakePanelTx.clear();
    for (int i = 0; i < 1000; i++) {
        panelLoop();
    }
    return fakePanelTx;
}

struct PanelFixture {
    PanelFixture() {
        fakeReset();
        fakePanelReset();
        panelInit();
    }
};

}

TEST_CASE_FIXTURE(PanelFixture, "init clears the panel and asks for its size")
{
    std::string tx = sendAll();
    CHECK(tx.find("\nC\n") == 0);
    CHECK(tx.find("?\n") != std::string::npos);
    CHECK(tx.find("T0L:\n") != std::string::npos);      // all lines sent, even empty
    CHECK(tx.find("T3L:\n") != std::string::npos);
}

TEST_CASE_FIXTURE(PanelFixture, "only changed lines are sent")
{
    sendAll();

    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, "GAMES1.ST");
    panelSetLine(2, PANEL_LEFT, 0, "host: 1.2.3.4");
    CHECK(sendAll() == "T0CB2:GAMES1.ST\nT2L:host: 1.2.3.4\n");

    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, "GAMES1.ST");     // same again
    CHECK(sendAll() == "");

    panelSetLine(0, PANEL_RIGHT, PANEL_DOUBLE | PANEL_BOLD, "GAMES1.ST");      // only justify changed
    CHECK(sendAll() == "T0RB2:GAMES1.ST\n");

    panelSetLine(4, PANEL_LEFT, 0, "x");                                       // no such line
    panelSetLine(-1, PANEL_LEFT, 0, "x");
    CHECK(sendAll() == "");
}

TEST_CASE_FIXTURE(PanelFixture, "the I message resends the whole screen, with scroll where the text doesn't fit")
{
    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE, "A_VERY_LONG_IMAGE_NAME.ST");  // 25 chars, panel has 18 in double
    panelSetLine(2, PANEL_LEFT, 0, "short");
    sendAll();

    fakePanelReceive("I1,4,25,18,BSD,0.1\n");
    std::string tx = sendAll();

    CHECK(panelPresent());
    CHECK(tx.find("\nC\n") == 0);
    CHECK(tx.find("T02S:A_VERY_LONG_IMAGE_NAME.ST\n") == std::string::npos);  // justify comes first
    CHECK(tx.find("T0C2S:A_VERY_LONG_IMAGE_NAME.ST\n") != std::string::npos);
    CHECK(tx.find("T2L:short\n") != std::string::npos);
    CHECK(tx.find("T1L:\n") != std::string::npos);
}

TEST_CASE_FIXTURE(PanelFixture, "a panel with fewer lines gets only its lines")
{
    fakePanelReceive("I1,1,12,6,B,0.1\n");
    sendAll();
    CHECK(panelLines() == 1);

    panelSetLine(0, PANEL_LEFT, 0, "GAMES1.ST");
    panelSetLine(1, PANEL_LEFT, 0, "host");
    CHECK(sendAll() == "T0L:GAMES1.ST\n");
}

TEST_CASE_FIXTURE(PanelFixture, "the uart is not waited for - at most what fits its fifo is written per loop")
{
    sendAll();
    panelSetLine(0, PANEL_LEFT, 0, "0123456789012345678901234");

    fakePanelTx.clear();
    panelLoop();
    CHECK(fakePanelTx.size() == 8);

    fakePanelWritable = 0;          // fifo full
    panelLoop();
    CHECK(fakePanelTx.size() == 8);

    fakePanelWritable = 8;
    for (int i = 0; i < 10; i++) {
        panelLoop();
    }
    CHECK(fakePanelTx == "T0L:0123456789012345678901234\n");
}

TEST_CASE_FIXTURE(PanelFixture, "flush sends right away, gives up after the timeout when the uart is stuck")
{
    sendAll();
    panelSetLine(1, PANEL_LEFT, 0, "wifi connecting");
    fakePanelTx.clear();
    panelFlush(50);
    CHECK(fakePanelTx == "T1L:wifi connecting\n");

    panelSetLine(1, PANEL_LEFT, 0, "connecting to host");
    fakePanelWritable = 0;
    uint32_t start = millis();
    panelFlush(50);
    CHECK(millis() - start >= 50);
    CHECK(millis() - start < 60);
}

TEST_CASE_FIXTURE(PanelFixture, "beeps")
{
    sendAll();
    panelBeep(50);
    panelBeep(100, 3200);
    CHECK(sendAll() == "B50\nB100,3200\n");
}

TEST_CASE_FIXTURE(PanelFixture, "text is cut to the max length, non-printable chars become spaces")
{
    sendAll();
    std::string longText(100, 'x');
    panelSetLine(3, PANEL_LEFT, 0, longText.c_str());
    CHECK(sendAll() == "T3L:" + std::string(PANEL_TEXT_MAX, 'x') + "\n");

    panelSetLine(3, PANEL_LEFT, 0, "a\nb\tc");
    CHECK(sendAll() == "T3L:a b c\n");
}

TEST_CASE_FIXTURE(PanelFixture, "key events are passed on, garbage is ignored")
{
    fakePanelReceive("KEH\nKLS\r\nK\nKEHX\nXYZ\n");
    fakePanelReceive(std::string(200, 'K') + "\nKRS\n");     // too long line dropped, the next one works
    panelLoop();
    REQUIRE(fakePanelKeys.size() == 3);
    CHECK(fakePanelKeys[0] == "EH");
    CHECK(fakePanelKeys[1] == "LS");
    CHECK(fakePanelKeys[2] == "RS");
}

TEST_CASE_FIXTURE(PanelFixture, "a bad I message is ignored")
{
    fakePanelReceive("I1,x\nI1,0,25,18,B,0.1\n");
    panelLoop();
    CHECK_FALSE(panelPresent());
    CHECK(panelLines() == PANEL_LINES);
}
