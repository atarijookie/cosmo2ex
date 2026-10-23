#ifndef PROTOCOL_H_
#define PROTOCOL_H_

// FDD -> panel messages, see protocol.md - hardware independent, so it's tested on the host (ce_pico_tests)

#include <cstdint>
#include <cstddef>

#define MAX_MSG_LEN         80                  // including the '\n'
#define MAX_TEXT_LEN        (MAX_MSG_LEN - 5)   // "T0L:" + '\n'
#define MAX_LINES           4

#define CMD_NONE            0
#define CMD_IDENTIFY        1   // ?
#define CMD_CLEAR           2   // C
#define CMD_TEXT            3   // T<n><j>[flags]:<text>
#define CMD_BEEP            4   // B<ms>[,<hz>]
#define CMD_BRIGHTNESS      5   // D<0-255>
#define CMD_BOOTLOADER      6   // !BOOT

#define JUSTIFY_LEFT        0
#define JUSTIFY_CENTER      1
#define JUSTIFY_RIGHT       2

#define TEXT_BOLD           0x01
#define TEXT_INVERTED       0x02
#define TEXT_DOUBLE         0x04
#define TEXT_SCROLL         0x08

typedef struct {
    uint8_t  type;
    uint8_t  line;          // CMD_TEXT
    uint8_t  justify;
    uint8_t  flags;
    char     text[MAX_TEXT_LEN + 1];
    uint16_t beepMs;        // CMD_BEEP
    uint16_t beepHz;
    uint8_t  brightness;    // CMD_BRIGHTNESS
} Command;

// collects chars into lines; too long lines are dropped up to the next '\n', '\r' is ignored
class LineReceiver
{
public:
    LineReceiver();
    bool feed(char c);                  // true when a whole line is in line()
    const char* line(void) const { return bfr; }

private:
    char   bfr[MAX_MSG_LEN];
    size_t len;
    bool   overflow;
};

bool parseCommand(const char* line, Command& cmd);     // false for unknown / invalid messages

#endif
