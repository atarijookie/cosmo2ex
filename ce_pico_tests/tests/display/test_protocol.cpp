// protocol.cpp of ce_pico_display - FDD -> panel messages, see ce_pico_display/protocol.md

#include <string>
#include <vector>

#include "doctest.h"

#include "defs.h"
#include "protocol.h"

namespace {

// feeds the chars, returns the completed lines
std::vector<std::string> feedAll(LineReceiver& rx, const std::string& data)
{
    std::vector<std::string> lines;
    for (char c : data) {
        if (rx.feed(c)) {
            lines.push_back(rx.line());
        }
    }
    return lines;
}

Command parse(const char* line, bool expectOk = true)
{
    Command cmd;
    CHECK(parseCommand(line, cmd) == expectOk);
    return cmd;
}

}

TEST_CASE("line receiver splits lines, ignores \\r")
{
    LineReceiver rx;
    auto lines = feedAll(rx, "?\r\nC\nT0L:abc\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "?");
    CHECK(lines[1] == "C");
    CHECK(lines[2] == "T0L:abc");
}

TEST_CASE("line receiver drops too long lines and resyncs on the next \\n")
{
    LineReceiver rx;
    std::string longLine(MAX_MSG_LEN, 'x');         // + '\n' = one more than allowed
    auto lines = feedAll(rx, longLine + "\nC\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "C");

    std::string maxLine(MAX_MSG_LEN - 1, 'y');      // exactly the max length with the '\n'
    lines = feedAll(rx, maxLine + "\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == maxLine);
}

TEST_CASE("simple commands")
{
    CHECK(parse("?").type == CMD_IDENTIFY);
    CHECK(parse("C").type == CMD_CLEAR);
    CHECK(parse("!BOOT").type == CMD_BOOTLOADER);

    parse("", false);
    parse("X", false);
    parse("?x", false);
    parse("!BOOTX", false);
}

TEST_CASE("text line")
{
    Command cmd = parse("T0C2:COSMO2EX");
    CHECK(cmd.type == CMD_TEXT);
    CHECK(cmd.line == 0);
    CHECK(cmd.justify == JUSTIFY_CENTER);
    CHECK(cmd.flags == TEXT_DOUBLE);
    CHECK(std::string(cmd.text) == "COSMO2EX");

    cmd = parse("T3RISB:12/40");
    CHECK(cmd.line == 3);
    CHECK(cmd.justify == JUSTIFY_RIGHT);
    CHECK(cmd.flags == (TEXT_INVERTED | TEXT_SCROLL | TEXT_BOLD));
    CHECK(std::string(cmd.text) == "12/40");

    cmd = parse("T2L:A: GAMES1.ST");           // ':' in the text
    CHECK(cmd.flags == 0);
    CHECK(std::string(cmd.text) == "A: GAMES1.ST");

    cmd = parse("T1L:");                        // empty = clear the line
    CHECK(std::string(cmd.text) == "");

    cmd = parse("T1LXB:x");                     // unknown flag ignored
    CHECK(cmd.flags == TEXT_BOLD);
}

TEST_CASE("invalid text lines")
{
    parse("T4L:x", false);      // only 4 lines
    parse("TaL:x", false);
    parse("T0X:x", false);      // bad justify
    parse("T0L", false);        // no ':'
    parse("T0Lx", false);
    parse("T", false);
}

TEST_CASE("no beep - the beeper is on the FDD board")
{
    parse("B50", false);
    parse("B50,2000", false);
}

TEST_CASE("brightness")
{
    Command cmd = parse("D128");
    CHECK(cmd.type == CMD_BRIGHTNESS);
    CHECK(cmd.brightness == 128);
    CHECK(parse("D0").brightness == 0);
    CHECK(parse("D255").brightness == 255);

    parse("D", false);
    parse("D256", false);
    parse("D1x", false);
}
