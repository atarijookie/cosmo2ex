#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "display.h"
#include "beeper.h"
#include "ipc.h"
#include "host_receive.h"

THeader fddHeader;      // keep the header global to preserve syncTag between calls
static bool headerPending = false;  // header received, but its data not handled yet - no free buffer for it until now

extern uint8_t imgTracks, imgSides, imgSectorsPerTrack;
extern char imageFileName[32];
extern bool diskChanged;
extern int imageState;

bool getIncommingHeader(void)
{
    while(true)
    {
        // Determine how many bytes are needed to be received, if we want to get 10 bytes of header.
        // If we already got some bytes in the syncTag, we need less than 10 bytes.
        int needed = 10;
        if((fddHeader.syncTag & 0xffffff) == (SYNC_TAG_FDD >> 8))      // got c050d1 (3 bytes) already? need only 7 more
        {
            needed = 7;
        }
        else if((fddHeader.syncTag & 0xffff) == (SYNC_TAG_FDD >> 16))  // got c050 (2 bytes) already? need only 8 more
        {
            needed = 8;
        }
        else if((fddHeader.syncTag & 0xff) == (SYNC_TAG_FDD >> 24))    // got c0 (1 bytes) already? need only 9 more
        {
            needed = 9;
        }
        else                // in other cases, expect to have all 10 bytes available before trying to read header
        {
            needed = 10;
        }

        // not enough data for full header, no header received
        int available = hostCanReadBytes();
        if(available < needed)
        {
            return false;
        }

        uint8_t data;
        hostRead(&data, 1);                             // read byte
        fddHeader.syncTag = fddHeader.syncTag << 8;     // shift previous sync tag one byte up
        fddHeader.syncTag |= data;                      // add lowest byte to syncTag

        if(fddHeader.syncTag == SYNC_TAG_FDD)           // found expected syncTag
        {
            uint8_t restOfHeader[6];
            hostRead(restOfHeader, 6);                  // read rest of the header

            fddHeader.cmdCode = getWord(restOfHeader);
            fddHeader.len = getDword(restOfHeader + 2);

            return true;                        // got complete header
        }
    }

    return false;       // no valid header
}

// Read the data part of the current message: the first bfrSize bytes into bfr, the rest of it is read and dropped,
// so the next header is searched for right after this message. Returns false if the data stopped coming before the
// whole data part was read - the connection is closed then, the reconnect requests the whole image again.
bool readMessageData(uint8_t* bfr, uint32_t bfrSize, uint32_t len)
{
    uint8_t dropBfr[64];
    uint32_t got = 0;
    uint32_t lastData = millis();

    while(got < len) {
        uint32_t available = hostCanReadBytes();

        if(available == 0) {        // nothing received now? wait a while for more, but not forever
            if(!hostConnected() || (millis() - lastData) >= MESSAGE_DATA_TIMEOUT_MS) {
                debug("readMessageData - got only %d of %d bytes, disconnecting\n", got, len);
                hostDisconnect();
                return false;
            }

            sleep_ms(1);
            continue;
        }

        uint32_t readSize = MIN(available, len - got);

        if(got < bfrSize) {         // still fits into bfr? read it there
            readSize = MIN(readSize, bfrSize - got);
            got += hostRead(bfr + got, readSize);
        } else {                    // beyond bfr, read and drop it
            readSize = MIN(readSize, sizeof(dropBfr));
            got += hostRead(dropBfr, readSize);
        }

        lastData = millis();
    }

    return true;
}

int receivedTracks = 0;

void showImageLoadProgress(void)
{
    static uint32_t lastDisplay = 0;

    uint32_t now = millis();
    if(now - lastDisplay < 200) {       // too soon after last display? quit
        return;
    }
    lastDisplay = now;

    uint32_t totalTracks = MAX(imgTracks * imgSides, 1);
    uint32_t percent = (receivedTracks * 100) / totalTracks;    // calc progress in percents

    char progress[32];
    memset(progress, 0, 32);
    for(int i=0; i<20; i++) {
        progress[i] = (percent > (i * 5)) ? '*' : ' ';
    }
    sprintf(progress + 20, " %3u%%", (unsigned) MIN(percent, 100));

    displayMessage("Loading image", imageFileName, progress);  // show on display
}

// Returns false if there's no free buffer for the track now - the message wasn't read, try again later.
bool handleTrackReceived(void)
{
    // don't wait for a buffer - until core1 frees one, the host waits (TCP) and the main loop goes on
    IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);
    if(bfr == NULL) {
        return false;
    }

    if(!readMessageData(bfr->data, bfr->maxDataSize, fddHeader.len)) {     // whole track not received? don't store a part of it
        ipcBufferRelease(bfr);
        return true;
    }

    if(fddHeader.len < 2) {     // no track # and side #? nothing to store
        debug("handleTrackReceived - too short: %d\n", fddHeader.len);
        ipcBufferRelease(bfr);
        return true;
    }

    if(bfr->data[0] >= MAX_TRACKS || bfr->data[1] > 1) {   // no such track / side? don't store it over another one
        debug("handleTrackReceived - bad track %d side %d\n", bfr->data[0], bfr->data[1]);
        ipcBufferRelease(bfr);
        return true;
    }

    bfr->length = MIN(bfr->maxDataSize, fddHeader.len);    // the part that was stored in the buffer

    // read track # and side # from bfr
    bfr->trackNo = bfr->data[0];
    bfr->sideNo = bfr->data[1];

    // store data to buffer, add buffer index to queue
    ipcSetBufferAndPutToFifo(bfr, 1, CMD_STORE_TRACK, bfr->length);

    debug("Rx t %d i %d\n", bfr->trackNo, bfr->sideNo);
    receivedTracks++;

    if(imageState == IMAGE_REQUESTED) {
        showImageLoadProgress();
    }

    return true;
}

void handleImageReceived(void)
{
    uint8_t tmpBfr[128];
    memset(tmpBfr, 0, sizeof(tmpBfr));

    if(!readMessageData(tmpBfr, sizeof(tmpBfr), fddHeader.len)) {
        return;
    }

    if(tmpBfr[0] == 1)     // image receiving finished?
    {
        diskChanged = true;
        imageState = IMAGE_LOADED;
        showRunningStateOnDisplay();
        beep(BEEP_IMAGE_LOADED_MS);
    }
    else                // image receiving started?
    {
        receivedTracks = 0;
        imageState = IMAGE_REQUESTED;
    }

    imgTracks = MIN(tmpBfr[1], MAX_TRACKS);
    imgSides = tmpBfr[2];
    imgSectorsPerTrack = MIN(tmpBfr[3], MAX_SECTORS_PER_TRACK);

    memset(imageFileName, 0, 32);
    strncpy(imageFileName, (const char*) (tmpBfr + 4), 31);        // store file name, up to 31 chars

    debug("handleImageReceived %s, imgTracks: %d, imgSides: %d, imageFileName: %s\n", (tmpBfr[0] == 1) ? "END" : "START", imgTracks, imgSides, imageFileName);
}

void handleIncommingData(void)
{
    if(!hostConnected()) {          // not connected? forget the message we were in, the next connection starts from scratch
        headerPending = false;
        fddHeader.syncTag = 0;
        return;
    }

    if(!headerPending) {
        if(!getIncommingHeader()) { // no valid header yet
            return;
        }

        fddHeader.syncTag = 0;      // clear sync tag
        headerPending = true;
    }

    switch(fddHeader.cmdCode) {
        case ATN_SEND_TRACK:
            headerPending = !handleTrackReceived();     // no free buffer? keep the header, try again next time
            break;

        case ATN_SEND_WHOLE_IMAGE:
            handleImageReceived();
            headerPending = false;
            break;

        default:
            debug("unknown cmdCode %x\n", fddHeader.cmdCode);
            readMessageData(NULL, 0, fddHeader.len);    // skip its data
            headerPending = false;
            break;
    }
}
