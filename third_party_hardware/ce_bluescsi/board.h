#ifndef __BOARD_H__
#define __BOARD_H__

#include <Arduino.h>

// The pico module on the BlueSCSI v2 board (2023.09a and newer layout, e.g. hardware/Desktop_50_Pin of BlueSCSI-v2):
// W6100-EVB-Pico / -Pico2 on the adapter (third_party_hardware/evb_to_w_adapter_for_bluescsi) or with 4 wires. The W6100
// sits on GP16..GP21, which are SCSI control lines on the BlueSCSI. These 6 EVB pins must not reach the BlueSCSI, the
// adapter moves the signals to GP10..GP13 (the SD card pins - the SD slot must stay empty). (Pico W / Pico 2 W with WiFi
// are not supported - not reliable enough.)
//
// GPIO   BlueSCSI net     EVB GPIO   meaning (target mode)
// 0..7   DB0..DB7         0..7       data, inverted (L on the GPIO = 1 on the bus)
// 8      DBP              8          parity
// 9      DBPTr            9          DATA_DIR - H: pico drives the data bus
// 18     oCD_iSEL         11         out: C/D while we hold BSY, in: SEL otherwise
// 19     oREQ             10         out: REQ (driven only while we hold BSY)
// 20     oMSG_iBSY        12         out: MSG while we hold BSY, in: BSY otherwise
// 21     oSEL (+iRST)     13         in: RST (L = bus reset), we never drive SEL
// 22     oIO              22         out: I/O (driven only while we hold BSY)
// 26     oACK (+iACK)     26         in: ACK (valid only while we hold BSY)
// 27     oBSY             27         out: BSY - L enables all our bus drivers
// 28     iATN / iMSG      28         in: ATN (valid only while we hold BSY)
// 16     SDA (Qwiic J6)   15         debug TX (the adapter wires it, J6 pin 3, GND is pin 1)
// 17     SCL (Qwiic J6)   14         not used
// -      -                25         LED of the EVB
//
// All the control signals are active L on the GPIOs.

struct TBoard {
    const char* name;

    uint8_t pinReq;         // out
    uint8_t pinCdSel;       // out C/D (while BSY) / in SEL
    uint8_t pinMsgBsy;      // out MSG (while BSY) / in BSY
    uint8_t pinRst;         // in RST
    uint8_t pinIo;          // out I/O
    uint8_t pinBsy;         // out BSY
    uint8_t pinAtn;         // in ATN

    uint8_t pinDebugTx;     // debug output, PIO UART TX at 115200
    uint8_t pinLed;         // 0xff = none
};

extern TBoard board;

void boardDetect(void);     // first thing in setup() - the pins of the board

#endif
