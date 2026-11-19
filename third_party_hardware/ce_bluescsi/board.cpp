#include "board.h"

TBoard board;

static const TBoard boardW6100Evb = {
#if defined(PICO_RP2350)
    "W6100-EVB-Pico2",
#else
    "W6100-EVB-Pico",
#endif
    10,     // REQ          - from BlueSCSI GP19
    11,     // C/D + SEL    - from BlueSCSI GP18
    12,     // MSG + BSY in - from BlueSCSI GP20
    13,     // RST          - from BlueSCSI GP21
    22,     // I/O
    27,     // BSY out
    28,     // ATN
    15,     // debug TX     - to BlueSCSI GP16 = Qwiic SDA (adapter)
    25      // LED of the EVB
};

// One board only - kept as a struct, so another pin mapping could be added (and picked here) later.
void boardDetect(void)
{
    board = boardW6100Evb;
}
