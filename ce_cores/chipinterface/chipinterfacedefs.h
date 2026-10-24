#ifndef __CHIPINTERFACEDEFS_H__
#define __CHIPINTERFACEDEFS_H__

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

// types of chip interface, as returned by the individual chip interface classes
#define CHIP_IF_DUMMY   -1
#define CHIP_IF_V1_V2   1
#define CHIP_IF_V3      3
#define CHIP_IF_V4      4
#define CHIP_IF_RASCSI  8
#define CHIP_IF_NETWORK 9

//---------------------------------------------
// ATN codes: commands sent from device to host (in header, see BufferedReader)

#define ATN_ANY                         0xff        // this is used only on host to wait for any ATN

// Hans (HDD)
#define ATN_FW_VERSION                  0x01        // followed by FW version, mac, features (shared with Franz, same code)
#define ATN_ACSI_COMMAND                0x02
#define ATN_READ_MORE_DATA              0x03
#define ATN_WRITE_MORE_DATA             0x04
// ATN_GET_STATUS                       0x05        // not used by current firmware

// Franz (FDD) - ATN_FW_VERSION (0x01) as above
#define ATN_SECTOR_WRITTEN              0x03        // sent: 3, side (highest bit) + track #, current sector #
#define ATN_SEND_TRACK                  0x04        // send the whole track
#define ATN_SEND_WHOLE_IMAGE            0x05        // send the whole image
#define ATN_GET_IMAGE_LIST              0x06        // page of image names in the folder of the current image, see fdd/imagelist.h
                                                    //   device: start (4 B, 0xffffffff = around the current image), count (2 B)
                                                    //   host (the reply, and on its own when the list / current image changes):
                                                    //   gen, total, current index, start (4 B each), count (2 B), count * [len (1 B), name]
#define ATN_INSERT_IMAGE                0x07        // insert image from that folder by name
                                                    //   device: gen, index (4 B each), name len (1 B), name
                                                    //   host: status (1 B) - INSERT_IMAGE_*

#define INSERT_IMAGE_OK                 0           // found, loading - the image follows as on ATN_SEND_WHOLE_IMAGE
#define INSERT_IMAGE_NOT_FOUND          1           // no such image in the folder (anymore)
#define INSERT_IMAGE_FAILED             2           // bad request, or the current image can't be browsed (no image, special image)

//---------------------------------------------
// CMD codes: commands sent from host to device

// Hans (HDD)
#define CMD_ACSI_CONFIG                 0x10
#define CMD_DATA_WRITE                  0x20
#define CMD_DATA_READ_WITH_STATUS       0x30
#define CMD_SEND_STATUS                 0x40
#define CMD_DATA_READ_WITHOUT_STATUS    0x50
#define CMD_FLOPPY_CONFIG               0x70
#define CMD_FLOPPY_SWITCH               0x80
#define CMD_GET_LICENSE                 0xa0
#define CMD_DO_UPDATE                   0xb0
#define CMD_DATA_MARKER                 0xda

// Franz (FDD)
#define CMD_WRITE_PROTECT_OFF           0x10
#define CMD_WRITE_PROTECT_ON            0x20
#define CMD_DISK_CHANGE_OFF             0x30
#define CMD_DISK_CHANGE_ON              0x40
#define CMD_SET_DRIVE_ID_0              0x70
#define CMD_SET_DRIVE_ID_1              0x80
#define CMD_DRIVE_ENABLED               0xa0
#define CMD_DRIVE_DISABLED              0xb0

//---------------------------------------------
// sizes

#define COMMAND_SIZE            10
#define ACSI_CMD_SIZE           14
#define WRITTENMFMSECTOR_SIZE   2048
#define TX_RX_BUFF_SIZE         600

// Most of the normal games have tracks from 11000 bytes to 12019 bytes of encoded stream.
// With Bad_Brew_Crew/BBC11.ZIP (11 sectors per track?) the resulting stream was 13205 bytes big, I didn't see anything more yet (but there might be something bigger).
// In theory the 11 sectors per track could be up to 13720 bytes big, so 13800 should be enough here.
// In case something bigger appears (e.g. 12 sectors per track), consider shortening GAP 2, GAP 3a, GAP 3B, GAP 4, which take 774 bytes before encoding, 1548 bytes after encoding,
// so if more space for real data would be needed, some adaptive reducing of GAPs sizes could be added (to a point where WD1772 still considers those GAPs to be valid to catch the sync there)
#define MFM_STREAM_SIZE         13800

#define MAKEWORD(A, B)  ( (((uint16_t)A)<<8) | ((uint16_t)B) )

#define HDD_FW_RESPONSE_LEN     12
#define FDD_FW_RESPONSE_LEN     8

#define FW_RESPONSE_LEN_BIGGER  ((HDD_FW_RESPONSE_LEN > FDD_FW_RESPONSE_LEN) ? HDD_FW_RESPONSE_LEN : FDD_FW_RESPONSE_LEN)

#define INBUF_SIZE  (WRITTENMFMSECTOR_SIZE + 8)

#define SYNC_TAG_HDD    0xc050d1c5
#define SYNC_TAG_FDD    0xc0500fdd

#define MAX_CLIENTS     8

#endif
