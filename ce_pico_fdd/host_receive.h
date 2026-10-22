#ifndef __HOST_RECEIVE_H__
#define __HOST_RECEIVE_H__

#include <cstdint>
#include <cstddef>

// Messages received from host on the fdd connection - tracks are passed to core1, image start / end handled here.

void handleIncommingData(void);     // handle the next message from host, if there is one

typedef struct {
    uint32_t syncTag;   //  0..3: 0xc0500fdd [COSmOFdd0] (4 bytes)
    uint16_t cmdCode;   //  4..5: cmd code (2 bytes)
    uint32_t len;       //  6..9: data len (4 bytes)
} THeader;

#define MESSAGE_DATA_TIMEOUT_MS     500     // max wait for more data of a message, before giving up on the connection

// fdd connection to host - in connection.cpp
bool hostConnected(void);
size_t hostCanReadBytes(void);
size_t hostRead(uint8_t* bfr, size_t size);     // read up to size bytes that were already received, don't wait
void hostDisconnect(void);
void showRunningStateOnDisplay(void);

#endif
