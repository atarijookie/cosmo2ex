#include "board.h"

TBoard board;

static const TBoard boardPicoW = {
    BOARD_PICO_W, "Pico W",
    19,     // REQ
    18,     // C/D + SEL
    20,     // MSG + BSY in
    21,     // RST
    22,     // I/O
    27,     // BSY out
    28,     // ATN
    16,     // debug TX - Qwiic SDA
    0xff    // LED on the WiFi chip
};

static const TBoard boardW6100Evb = {
    BOARD_W6100_EVB, "W6100-EVB-Pico",
    10,     // REQ          - wire from BlueSCSI GP19
    11,     // C/D + SEL    - wire from BlueSCSI GP18
    12,     // MSG + BSY in - wire from BlueSCSI GP20
    13,     // RST          - wire from BlueSCSI GP21
    22,     // I/O
    27,     // BSY out
    28,     // ATN
    25,     // debug TX - the LED pin of the module (not on its edge pins)
    0xff    // no free LED pin - the debug output blinks GP25
};

// The arduino core decides before setup() whether the WiFi chip is there and starts it:
// - RP2040 built for Pico W: checked at runtime (GP29 / GP25), so this build runs also on a plain Pico / W6100-EVB-Pico.
// - RP2350 built for Pico 2 W: no runtime check, the WiFi chip is always assumed - the W6100-EVB-Pico2 will need its
//   own build (board: Raspberry Pi Pico 2).
void boardDetect(void)
{
    board = rp2040.isPicoW() ? boardPicoW : boardW6100Evb;
}
