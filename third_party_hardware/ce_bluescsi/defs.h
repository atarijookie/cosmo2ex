#ifndef DEFS_H_
#define DEFS_H_

#include <Arduino.h>

#define LOG_MORE    1
#define LOG_PIO     1       // debug output: PIO UART TX at 115200 on board.debugTx (see board.cpp)

#ifndef MIN
    #define MIN(X,Y)    ((X < Y) ? X : Y)
#endif

#ifndef MAX
    #define MAX(X,Y)    ((X > Y) ? X : Y)
#endif

#define DEV_FEATURE_IKBD    8
#define DEV_FEATURE_FDD     4
#define DEV_FEATURE_SCSI    2
#define DEV_FEATURE_ACSI    1

#define CMD_TIMEOUT_SECS_PER_MB 3
#define CMD_TIMEOUT_ONESECOND   1000
#define CMD_TIMEOUT_SHORT       (CMD_TIMEOUT_ONESECOND / 2)     // this period will be 0.5 second
#define CMD_TIMEOUT_LONG        (CMD_TIMEOUT_ONESECOND * 3)     // this period will be 3.0 second

// Over WiFi the host's answer (or the next chunk of it) sometimes comes 0.2 - 1.5 s late (TCP retransmits). The TT gives the
// data phase 3 s (SCSI_TIMEOUT_LONG in libacsiscsi), so wait for the host almost that long - ce_pico_hdd's 0.5 s is too short.
#define CMD_TIMEOUT_HOST        2500

// commands sent from device to host (the same protocol as ce_pico_hdd)
#define ATN_FW_VERSION                      0x01                                // followed by string with FW version (length: 4 uint16_ts - cmd, v[0], v[1], 0)
#define ATN_ACSI_COMMAND                    0x02
#define ATN_WRITE_MORE_DATA                 0x04
#define ATN_FW_INFO                         0x0a    // identity, size + MD5 of the running image, FS position - once per connection, see fw_update.cpp
#define ATN_FW_UPDATE_STATUS                0x0b    // answer to each CMD_FW_UPDATE_* - see fw_update.cpp

// commands sent from host to device
#define CMD_ACSI_CONFIG                     0x10
#define CMD_DATA_WRITE                      0x20
#define CMD_DATA_READ_WITH_STATUS           0x30
#define CMD_SEND_STATUS                     0x40
#define CMD_DATA_READ_WITHOUT_STATUS        0x50
#define CMD_DATA_MARKER                     0xda
#define CMD_LED_COLORS                      0xe0    // neopixel colors from the web UI - no neopixel here, ignored

// firmware update (OTA) from the web UI - see fw_update.cpp; the device answers each with ATN_FW_UPDATE_STATUS
#define CMD_FW_UPDATE_START                 0xf0    // image size (4 B), MD5 of the image (16 B)
#define CMD_FW_UPDATE_DATA                  0xf1    // offset (4 B), data (max FWU_CHUNK_MAX B)
#define CMD_FW_UPDATE_END                   0xf2    // check identity, layout, MD5, then reboot into the OTA stub
#define CMD_FW_UPDATE_ABORT                 0xf3

// these states define if the device should get command or transfer data
#define STATE_GET_COMMAND                       0
#define STATE_WAIT_COMMAND_RESPONSE             2
#define STATE_DATA_READ_WITH_STATUS             3
#define STATE_DATA_READ_WITHOUT_STATUS          4
#define STATE_DATA_WRITE                        5
#define STATE_WAIT_FOR_STATUS_ARRIVAL           6
#define STATE_READ_STATUS                       7

#define STATE_SEND_FW_VER                       10
#define STATE_SEND_WRITE_MORE_DATA              11
#define STATE_DATA_READ                         12

///////////////////////////

#define TX_HEADER_SIZE                  10
#define ATN_SENDFWVERSION_LEN_TX        (TX_HEADER_SIZE + 12)
#define ATN_SENDACSICOMMAND_LEN_TX      (TX_HEADER_SIZE + 14)

///////////////////////////
// SCSI pins - see board.h, most of them depend on the board (Pico W / 2W or the rewired W6100-EVB-Pico)

#define PIN_D0              0       // D0..D7 + DBP on GP0..GP8 through the LVT245s, DATA_DIR sets their direction
#define PIN_DBP             8
#define PIN_DATA_DIR        9       // out - H: pico drives the bus (only while we hold BSY)
#define PIN_ACK             26      // in  - ACK from the initiator (readable only while we hold BSY)

#define SCSI_DATA_PINS_MASK     0x1ff                   // D0..D7 + DBP

#define BIT_IS_H(PIN)   (gpio_get(PIN) != 0)
#define BIT_IS_L(PIN)   (gpio_get(PIN) == 0)

#endif /* DEFS_H_ */
