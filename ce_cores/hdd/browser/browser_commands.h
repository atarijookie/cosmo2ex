#ifndef _BROWSER_COMMANDS_H_
#define _BROWSER_COMMANDS_H_

// Commands of the CosmosEx browser module (HOSTMOD_BROWSER), sent by CE_BROWS.PRG. The page is rendered on the host
// by ce_browser (../../../ce_browser, headless Chromium), the ST only copies the received pixels to the screen and
// sends the input back. Keep in sync with the copy of this file used by CE_BROWS.PRG.
//
// Command format, pCmd as BrowserModule::processCommand() gets it (for ICD commands the 1st byte is already skipped):
//   [0] xx  [1] 'C' [2] 'E' [3] HOSTMOD_BROWSER [4] cmd [5..13] args
// Multi byte values are big endian. The status byte is BRW_OK or BRW_ERR_*.
// SET_MODE and NAVIGATE succeed even when ce_browser isn't connected - the host keeps them and sends them when it
// connects (also after ce_browser restarted). Input and NAV_ACTION fail with BRW_ERR_NOT_RUNNING then.

#define BROWSER_MODULE_VERSION          0x0100      // in IDENTIFY reply

// status codes
#define BRW_OK                          0x00
#define BRW_ERR_NOT_RUNNING             0x01        // ce_browser is not connected (starting, crashed, not installed)
#define BRW_ERR_BAD_PARAM               0x02        // unknown resolution / action, page size out of range, ...
#define BRW_ERR_NO_MODE                 0x03        // GET_UPDATE before SET_MODE
#define BRW_ERR_INTERNAL                0x04        // data transfer with the ST failed

// commands
#define BRW_CMD_IDENTIFY                0x00        // reply (48 B): "CosmosEx browser module" (24 B), padding to 32,
                                                    //   version (W @ 32), flags (B @ 34: BRW_IDF_*), padding.
                                                    //   Also starts ce_browser if it isn't running.
#define BRW_CMD_SET_MODE                0x01        // [5] SCREENCAST_RES_*, [6..7] page area height in pixels (screen height
                                                    //   minus the toolbar), [8] page zoom in quarters (4 = 1x, 8 = 2x, 0 = 1x).
                                                    //   Page width = screen width. The next update is a full frame.
#define BRW_CMD_NAVIGATE                0x02        // data to host: 512 B, URL or search text, zero terminated
#define BRW_CMD_NAV_ACTION              0x03        // [5] BRW_NAV_*
#define BRW_CMD_MOUSE                   0x10        // [5] BRW_MOUSE_*, [6..7] x, [8..9] y (page area pixels), [10] buttons (bit 0 left, bit 1 right)
#define BRW_CMD_KEY                     0x11        // [5] scancode, [6] ASCII (0 for non-printing keys), [7] shift state (Kbshift())
#define BRW_CMD_SCROLL                  0x12        // [5] lines down, [6] pages down, [7] columns right, [8] pages right
                                                    //   (all signed - negative = up / left; line / column = 40 CSS pixels)
#define BRW_CMD_GET_UPDATE              0x20        // [5..8] seq of the frame which is completely on the ST's screen (0 = none),
                                                    //   [9] max sectors of the reply (1 .. 254), [10..11] row to resume from
                                                    //   (0 = new update; else the last reply had BRW_UPD_MORE and this is
                                                    //   rect.y + rect.h of it). Reply: see below.
#define BRW_CMD_GET_STATUS              0x21        // reply (512 B): see below
#define BRW_CMD_GET_HISTORY             0x22        // [5] max URLs (0 = all), [6] max sectors of the reply (1 .. 254).
                                                    //   Reply: count (W), then the URLs zero terminated, newest first -
                                                    //   pages which loaded successfully, each once. As many as fit.

// IDENTIFY flags
#define BRW_IDF_CONNECTED               0x01        // ce_browser is connected

// NAV_ACTION actions
#define BRW_NAV_BACK                    0
#define BRW_NAV_FORWARD                 1
#define BRW_NAV_RELOAD                  2
#define BRW_NAV_STOP                    3
#define BRW_NAV_HOME                    4
#define BRW_NAV_LAST                    BRW_NAV_HOME

// MOUSE events
#define BRW_MOUSE_MOVE                  0
#define BRW_MOUSE_DOWN                  1
#define BRW_MOUSE_UP                    2
#define BRW_MOUSE_LAST                  BRW_MOUSE_UP

// GET_UPDATE reply, padded to a multiple of 16 bytes, every part longword aligned:
//   header (16 B):
//     [0..3]   seq of the frame these pixels belong to
//     [4]      flags BRW_UPD_*
//     [5]      SCREENCAST_RES_* of the frame
//     [6..7]   rect count
//     [8..9]   palette entry count (0 = no palette in this reply)
//     [10..11] page area width, [12..13] page area height (pixels)
//     [14]     bitplanes, [15] zoom in quarters
//   palette: entry count x W, as written to the palette registers (ST: 0x0RGB, 3 bits each; TT: 0x0RGB, 4 bits each)
//   rects, each:
//     [0..1] x in 16 pixel units, [2..3] y in pixels, [4..5] width in 16 pixel units, [6..7] height in pixels
//     then height rows of (width x bitplanes) words, interleaved bitplanes as in the screen memory
// After the reply without BRW_UPD_MORE, the frame 'seq' is completely on the screen - send it as [5..8] next time.
#define BRW_UPD_HEADER_SIZE             16
#define BRW_UPD_RECT_HEADER_SIZE        8

#define BRW_UPD_PALETTE                 0x01        // palette follows the header
#define BRW_UPD_MORE                    0x02        // more of this frame waits - call GET_UPDATE again right away with [10..11] = next row
#define BRW_UPD_FULL                    0x04        // the rects of this frame (with all its parts) cover the whole page area
#define BRW_UPD_NO_CHANGE               0x08        // nothing new since the frame in [5..8], no rects

// GET_STATUS reply (512 B):
//   [0]        flags BRW_STF_*
//   [1]        page load progress, 0 .. 100
//   [2..3]     caret x, [4..5] caret y in page area pixels (0xffff = no text field has focus)
//   [6..7]     reserved
//   [8..11]    seq of the newest frame on the host (0 = none)
//   [12..15]   reserved
//   [16..271]  URL (zero terminated)
//   [272..399] page title (zero terminated)
//   [400..511] message for the status line (zero terminated): errors, "ce_browser is not running", ...
#define BRW_STATUS_SIZE                 512
#define BRW_STATUS_URL_OFFSET           16
#define BRW_STATUS_URL_SIZE             256
#define BRW_STATUS_TITLE_OFFSET         272
#define BRW_STATUS_TITLE_SIZE           128
#define BRW_STATUS_MSG_OFFSET           400
#define BRW_STATUS_MSG_SIZE             112

#define BRW_STF_CONNECTED               0x01        // ce_browser is connected
#define BRW_STF_LOADING                 0x02
#define BRW_STF_CAN_BACK                0x04
#define BRW_STF_CAN_FORWARD             0x08

#define BRW_URL_SIZE                    512         // NAVIGATE data size
#define BRW_HISTORY_MAX                 100         // URLs kept in the history

#endif
