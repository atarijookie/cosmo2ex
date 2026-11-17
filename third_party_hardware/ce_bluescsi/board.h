#ifndef __BOARD_H__
#define __BOARD_H__

#include <Arduino.h>

// The pico module on the BlueSCSI v2 board (2023.09a and newer layout, e.g. hardware/Desktop_50_Pin of BlueSCSI-v2):
//
// - Pico W / Pico 2 W: plugged in as is, network over WiFi.
// - W6100-EVB-Pico / -Pico2: the W6100 sits on GP16..GP21, which are SCSI control lines on the BlueSCSI. These 6 header
//   pins must stay unconnected, and 4 wires move the signals to GP10..GP13 (the SD card pins - the SD slot must stay
//   empty). Network over the W6100 - NOT IMPLEMENTED YET.
//
// GPIO   BlueSCSI net     Pico W / 2W   W6100-EVB   meaning (target mode)
// 0..7   DB0..DB7         0..7          0..7        data, inverted (L on the GPIO = 1 on the bus)
// 8      DBP              8             8           parity
// 9      DBPTr            9             9           DATA_DIR - H: pico drives the data bus
// 18     oCD_iSEL         18            11          out: C/D while we hold BSY, in: SEL otherwise
// 19     oREQ             19            10          out: REQ (driven only while we hold BSY)
// 20     oMSG_iBSY        20            12          out: MSG while we hold BSY, in: BSY otherwise
// 21     oSEL (+iRST)     21            13          in: RST (L = bus reset), we never drive SEL
// 22     oIO              22            22          out: I/O (driven only while we hold BSY)
// 26     oACK (+iACK)     26            26          in: ACK (valid only while we hold BSY)
// 27     oBSY             27            27          out: BSY - L enables all our bus drivers
// 28     iATN / iMSG      28            28          in: ATN (valid only while we hold BSY)
// 16/17  SDA / SCL        16            -           Qwiic connector J6 - debug TX on SDA (J6 pin 3, GND is pin 1)
//
// All the control signals are active L on the GPIOs.

#define BOARD_PICO_W        0       // Pico W / Pico 2 W - WiFi
#define BOARD_W6100_EVB     1       // W6100-EVB-Pico / -Pico2 with the 4 wires - W6100

struct TBoard {
    uint8_t type;           // BOARD_*
    const char* name;

    uint8_t pinReq;         // out
    uint8_t pinCdSel;       // out C/D (while BSY) / in SEL
    uint8_t pinMsgBsy;      // out MSG (while BSY) / in BSY
    uint8_t pinRst;         // in RST
    uint8_t pinIo;          // out I/O
    uint8_t pinBsy;         // out BSY
    uint8_t pinAtn;         // in ATN

    uint8_t pinDebugTx;     // debug output, PIO UART TX at 115200
    uint8_t pinLed;         // 0xff = the LED of the WiFi chip (LED_BUILTIN)
};

extern TBoard board;

void boardDetect(void);     // first thing in setup()

#define BOARD_IS_WIFI   (board.type == BOARD_PICO_W)

#endif
