#ifndef _BROWSERLINK_PROTOCOL_H_
#define _BROWSERLINK_PROTOCOL_H_

// Messages between ce_cores (BrowserLink) and ce_browser, on a TCP connection on 127.0.0.1. ce_cores listens on
// a free port and starts 'ce_browser serve -connect 127.0.0.1:<port>'; ce_browser connects to it.
// Keep in sync with ../../../ce_browser (link.go).
//
// Each message: type (B), payload length (DW), payload. Multi byte values are big endian.

#define BLINK_VERSION               1
#define BLINK_HEADER_SIZE           5
#define BLINK_MAX_PAYLOAD           (2 * 1024 * 1024)   // a bigger length means a broken stream - the connection is closed

// ce_cores -> ce_browser
#define BLINK_HELLO                 0x01    // version (W)
#define BLINK_SET_MODE              0x02    // res (B, SCREENCAST_RES_*), zoom in quarters (B), page width (W), page height (W), bitplanes (B)
#define BLINK_NAVIGATE              0x03    // URL or search text (UTF-8 / Atari charset as typed, no terminator)
#define BLINK_NAV_ACTION            0x04    // action (B, BRW_NAV_*)
#define BLINK_MOUSE                 0x05    // event (B, BRW_MOUSE_*), x (W), y (W), buttons (B)
#define BLINK_KEY                   0x06    // scancode (B), ASCII (B), shift state (B)
#define BLINK_SCROLL                0x07    // lines down, pages down, columns right, pages right (B each, signed)

// ce_browser -> ce_cores
#define BLINK_HELLO_REPLY           0x81    // version (W)
#define BLINK_FRAME                 0x82    // seq (DW, never 0, increasing), res (B), zoom (B), width (W), height (W),
                                            // bitplanes (B), reserved (B), palette count (W), palette (count x W),
                                            // bitplane data: height rows of width / 16 x bitplanes words (interleaved)
#define BLINK_STATUS                0x83    // flags (B, BRW_STF_* without CONNECTED), progress (B), caret x (W), caret y (W),
                                            // URL length (W) + URL, title length (W) + title, message length (W) + message
#define BLINK_HISTORY_ADD           0x84    // URL of a page which loaded successfully (no terminator)

#define BLINK_FRAME_HEADER_SIZE     14      // up to the palette

#endif
