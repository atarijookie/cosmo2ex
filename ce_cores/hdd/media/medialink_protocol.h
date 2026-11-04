#ifndef _MEDIALINK_PROTOCOL_H_
#define _MEDIALINK_PROTOCOL_H_

// Messages between ce_cores (MediaLink) and ce_media, on a TCP connection on 127.0.0.1. ce_cores listens on a free
// port and starts 'ce_media serve -connect 127.0.0.1:<port> ...'; ce_media connects to it. Another producer of
// streams (e.g. a game running on the host) can connect the same way and speak the same messages.
// Keep in sync with ../../../ce_media (link.go).
//
// Each message: type (B), payload length (DW), payload. Multi byte values are big endian.
// The INFO block, YM table and chunks are already in the format the ST gets (media_commands.h) - ce_cores only keeps
// them and hands them out.

#define MLINK_VERSION               2           // 2: MLINK_TAKEN
#define MLINK_HEADER_SIZE           5
#define MLINK_MAX_PAYLOAD           (512 * 1024)        // a bigger length means a broken stream - the connection is closed

// ce_cores -> ce_media
#define MLINK_HELLO                 0x01    // version (W)
#define MLINK_OPEN                  0x02    // generation (DW), the first 16 B of the OPEN data (MED_OPEN_*), host path
                                            // (rest of the payload, UTF-8, no terminator) - stops the previous stream
#define MLINK_SEEK                  0x03    // generation (DW), position ms (DW) - restart the stream there
#define MLINK_INPUT                 0x04    // scancodes (B each)
#define MLINK_CLOSE                 0x05    // stop the stream
#define MLINK_TAKEN                 0x06    // generation (DW), seq (DW) - the ST took this chunk of a live stream: ce_media
                                            // sends a new one only then (at most 2 on the way), so nothing piles up
                                            // between it and the ST - a game stays as fresh as the ST can take it

// ce_media -> ce_cores
#define MLINK_HELLO_REPLY           0x81    // version (W)
#define MLINK_INFO                  0x82    // MED_INFO_SIZE bytes - the GET_INFO reply
#define MLINK_YM_TABLE              0x83    // MED_YM_TABLE_SIZE bytes
#define MLINK_CHUNK                 0x84    // the chunk as the ST gets it (generation and seq are in its header)

#endif
