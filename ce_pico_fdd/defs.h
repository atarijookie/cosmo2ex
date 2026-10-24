#ifndef DEFS_H_
#define DEFS_H_

#include <cstdint>

#define LOG_MORE    1

#ifndef TRUE
    #define TRUE 1
#endif

#ifndef FALSE
    #define FALSE 0
#endif

#ifndef MIN
    #define MIN(X,Y)    ((X < Y) ? X : Y)
#endif

#ifndef MAX
    #define MAX(X,Y)    ((X > Y) ? X : Y)
#endif

#define MFM_4US         1
#define MFM_6US         2
#define MFM_8US         3

#define DEV_FEATURE_IKBD    8
#define DEV_FEATURE_FDD     4
#define DEV_FEATURE_SCSI    2
#define DEV_FEATURE_ACSI    1

// commands sent from device to host
#define ATN_FW_VERSION          0x01        // followed by string with FW version (length: 4 WORDs - cmd, v[0], v[1], 0)
#define ATN_SECTOR_WRITTEN      0x03        // sent: 3, side (highest bit) + track #, current sector #
#define ATN_SEND_TRACK          0x04        // send the whole track
#define ATN_SEND_WHOLE_IMAGE    0x05        // send the whole image
#define ATN_GET_IMAGE_LIST      0x06        // page of image names in the folder of the current image - see image_browser.h
#define ATN_INSERT_IMAGE        0x07        // insert image from that folder by name, the host replies with INSERT_IMAGE_*

#define INSERT_IMAGE_OK         0           // found, loading - the image follows as on ATN_SEND_WHOLE_IMAGE
#define INSERT_IMAGE_NOT_FOUND  1           // no such image in the folder (anymore)
#define INSERT_IMAGE_FAILED     2           // bad request, or the current image can't be browsed

// commands sent from host to device
#define CMD_WRITE_PROTECT_OFF       0x10
#define CMD_WRITE_PROTECT_ON        0x20
#define CMD_DISK_CHANGE_OFF         0x30
#define CMD_DISK_CHANGE_ON          0x40
#define CMD_CURRENT_SECTOR          0x50                            // followed by sector #
#define CMD_GET_FW_VERSION          0x60
#define CMD_SET_DRIVE_ID_0          0x70
#define CMD_SET_DRIVE_ID_1          0x80
#define CMD_CURRENT_TRACK           0x90                            // followed by track #
#define CMD_DRIVE_ENABLED           0xa0
#define CMD_DRIVE_DISABLED          0xb0
#define CMD_DATA_PART_OF_SECTOR     0xc0

#define CMD_TRACK_STREAM_END        0xF0                            // this is the mark in the track stream that we shouldn't go any further in the stream

///////////////////////////

#define TX_HEADER_SIZE                  10
#define ATN_SENDFWVERSION_LEN_TX        (TX_HEADER_SIZE + 12)
#define ATN_SENDTRACK_REQ_LEN_TX        (TX_HEADER_SIZE + 2)

///////////////////////////

#define SPI_PORT spi0

#define PANEL_UART_BAUD     115200


#define PIN_TXD_DEBUG       0
#define PIN_CAPTIVE_BTN     1       // button to 3.3V (H = pressed), long press starts the captive portal - instead of the debug UART RX, which isn't used
#define PIN_NEOPIXEL        1       // the same pin, to LED2 through the AND gate IC6 - output only while sending a frame, see neopixel_pio.cpp
#define PIN_DRIVE_SEL       2
#define PIN_MOT_EN          3
#define PIN_DIR             4
#define PIN_STEP            5
#define PIN_WGATE           6
#define PIN_SIDE1           7
#define PIN_KEYB_TX         8
#define PIN_KEYB_TX_ORIG    9
#define PIN_SCK             10
#define PIN_MOSI            11
#define PIN_MISO            12
#define PIN_CS              13
#define PIN_RDATA           14
#define PIN_DSKCHG          15
#define PIN_PANEL_TX        16      // front panel UART (PIO), see ce_pico_display/protocol.md
#define PIN_PANEL_RX        17
#define PIN_KEYB_RX         18
#define PIN_WPROTECT        19
#define PIN_TRACK00         20
#define PIN_INDEX           21
#define PIN_DENSITY         22
#define PIN_WDATA           26
#define PIN_BEEPER          27      // buzzer with its own generator, H = on, see beeper.h
#define PIN_FLCC_OE         28

#define BIT_IS_H(PIN)   (gpio_get(PIN) != 0)
#define BIT_IS_L(PIN)   (gpio_get(PIN) == 0)
#define BIT_LEVEL(PIN)  ( (gpio_get(PIN) == 0) ? 0 : 1)

#define WRITEBUFFER_SIZE    1300

typedef struct {
    volatile uint8_t side;
    volatile uint8_t track;
    volatile uint8_t sector;
} TDrivePosition;

#define READTRACKDATA_SIZE_BYTES    13800
#define MAX_TRACKS                  90
#define MAX_SECTORS_PER_TRACK       11

#define ENCODED_SECTOR_MAX_SIZE     1200    // the mfm encoded sector - header + gaps + markers + data - should not exceed this size. Using fixed size to simplify sector write to memory in device.

#define IMAGE_NOT_LOADED    0
#define IMAGE_REQUESTED     1
#define IMAGE_LOADED        2

#define STREAM_TABLE_ITEMS  20
#define STREAM_TABLE_SIZE   STREAM_TABLE_ITEMS

#define STREAM_START_OFFSET STREAM_TABLE_SIZE

#define TAG_WRITE_START 0x80    // start of sector data - sent from MFM streamer
#define TAG_WRITE_END   0xc0    // end of sector data - sent from MFM streamer

#endif /* DEFS_H_ */
