#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>

#include "../misc/utils.h"
#include "../misc/debug.h"
#include "../misc/global.h"
#include "bufferedreader.h"
#include "chipinterface.h"

BufferedReader::BufferedReader()
{
    fd = FD_EMPTY;
    whichLog = LOGFILE_HDD;
    gotBytes = 0;
    dataSizeBytes = 0;
    skippedBytes = 0;
    setSyncTag(SYNC_TAG_HDD);
}

BufferedReader::~BufferedReader()
{

}

void BufferedReader::setSyncTag(uint32_t whichSyncTagCode)
{
    whichSyncTag = whichSyncTagCode;

    // header with this sync tag comes from this chip
    switch(whichSyncTag) {
        case SYNC_TAG_HDD:  chipAtnId = NET_ATN_HANS_ID;    break;
        case SYNC_TAG_FDD:  chipAtnId = NET_ATN_FRANZ_ID;   break;
        default:            chipAtnId = NET_ATN_NONE_ID;    break;  // unknown sync tag - no header will be reported as valid
    }
}

void BufferedReader::setLogFile(int whichLogFile)
{
    whichLog = whichLogFile;
}

void BufferedReader::setFd(int inFd)
{
    fd = inFd;
}

int BufferedReader::waitForAtn(uint8_t atnCode, uint32_t timeoutMs)
{
    if(fd < 0) {                    //  no fd, no ATN
        return NET_ATN_NONE_ID;
    }

    bool waitedOnce = false;                                // when timeoutMs says don't wait for data, make it wait once for a short time to be able to detect client socket disconnect
    Timeout timeout(timeoutMs);                             // when this timeout expires and no data is available, quit

    while(sigintReceived == 0) {
        // got data? process
        if(gotBytes == HEADER_BUFFER_SIZE) {                // have enough data?
            int atnId = readHeaderFromBuffer(atnCode);

            if(atnId != NET_ATN_NONE_ID) {                  // if valid ATN ID found and header seems to be OK, return that ATN ID
                if(skippedBytes > 0) {                      // had to skip some garbage before this header? log it once, not per each byte
                    Debug::out(whichLog, LOG_DEBUG, "waitForAtn() - resynced, skipped %d bytes before valid header", skippedBytes);
                    skippedBytes = 0;
                }

                return atnId;
            }

            // no known tag string or header invalid? remove 1 byte from begining, start loop over again
            popFirst();
            skippedBytes++;
            continue;
        }

        // Not enough data in buffer?
        // check if data was received and is present in socket by using ioctl().
        // if data is  available, the rest does: ioctl()            + recv()
        // if data not available, the rest does: ioctl() + select() + recv()

        // we need HEADER_BUFFER_SIZE bytes to have full header
        int needCnt = HEADER_BUFFER_SIZE - gotBytes;

        int res, bytesAvailable;
        res = ioctl(fd, FIONREAD, &bytesAvailable);     // how many bytes we can read immediately?

        if(res < 0) {                                   // ioctl() failed? no bytes available
            bytesAvailable = 0;
        }

        if(bytesAvailable <= 0) {                       // if no bytes available? wait at least once
            uint64_t timeLeftUs = (uint64_t) timeout.leftMs() * 1000;  // how much time is left in us; 0 if timeout expired

            if(timeoutMs == 0 && !waitedOnce) {         // if this is the wait for 1st ATN (with 0 wait time), but didn't wait at least once, wait at least once for a short time
                waitedOnce = true;                      // now were about to wait once
                timeLeftUs = 1000;                      // just a short 1 ms wait
            }

            if(timeLeftUs <= 0) {                       // no time left? quit loop, return NONE ATN (normal outcome, don't log)
                break;
            }

            struct timeval tv;
            tv.tv_sec = timeLeftUs / 1000000;           // split timeout into s and us, as tv_usec must be < 1000000
            tv.tv_usec = timeLeftUs % 1000000;

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            res = select(fd + 1, &readfds, NULL, NULL, &tv);     // wait for data or timeout here

            if(res < 0 || !FD_ISSET(fd, &readfds)) {    // if select() failed or cannot read from fd, skip rest
                continue;
            }
        }

        // If got here, then either ioctl() told us we got some bytes available,
        // or select() told us we can recv() from socket now.

        ssize_t recvCnt = recv(fd, &buffer[gotBytes], needCnt, 0);

        if(recvCnt == 0) {                              // if recv() returned 0, then client disconnected
            Debug::out(whichLog, LOG_DEBUG, "waitForAtn() - recvCount=0, returning NET_ATN_DISCONNECTED");
            return NET_ATN_DISCONNECTED;
        }

        if(recvCnt > 0) {                               // if read was OK, we got those bytes and we can restart the loop
            gotBytes += recvCnt;
            // logHdd(LOG_DEBUG, "waitForAtn() - received data, gotBytes: %d", gotBytes);
            continue;
        }

        // on failed to get data code continues here, try the loop again
    }

    //logHdd(LOG_DEBUG, "waitForAtn() - quitting, returning NET_ATN_NONE_ID");
    return NET_ATN_NONE_ID;     // nothing usable found
}

void BufferedReader::popFirst(void)
{
    if(gotBytes < 1) {              // no data? nothing to do
        return;
    }

    gotBytes--;                     // we got now 1 byte less

    int i;
    for(i=0; i<gotBytes; i++) {     // move bytes to lower index
        buffer[i] = buffer[i + 1];
    }
}

int BufferedReader::readHeaderFromBuffer(uint8_t atnCodeWant)
{
    // The buffer should contain:
    //  0..3: 0xc050d1c5 [COSmODICS] (4 bytes)
    //  4..5: ATN code (2 bytes)
    //  6..9: rest of the data size in bytes (4 bytes)
    // total: 10 bytes (HEADER_BUFFER_SIZE)

    if(gotBytes < HEADER_BUFFER_SIZE) {     // should have enough data to check them
        return NET_ATN_NONE_ID;
    }

    uint32_t syncDword = Utils::getDword(&buffer[0]);
    if(syncDword != whichSyncTag) {                       // sync bytes wrong? happens for each byte while resyncing, caller logs summary
        return NET_ATN_NONE_ID;
    }

    if(atnCodeWant != ATN_ANY) {                                // if it's not this special value, we're waiting for specific ATN code
        uint8_t atnCodeGot = getAtnCode();

        if(atnCodeGot != atnCodeWant) {                         // the ATN code (command) is wrong, fail
            Debug::out(whichLog, LOG_DEBUG, "readHeaderFromBuffer() - wanted atnCodeWant: %d, but got atnCodeGod: %d", atnCodeWant, atnCodeGot);
            return NET_ATN_NONE_ID;
        }
    }

    // read dataSizeBytes
    dataSizeBytes = Utils::getDword(&buffer[6]);

    //logHdd(LOG_DEBUG, "readHeaderFromBuffer() - got AtnCode=%d, txLen=%d, rxLen=%d", getAtnCode(), txLen, rxLen);

    // value other than NET_ATN_NONE_ID means success
    return chipAtnId;
}

// from the current buffer gets and returns the ATN code found in header
uint8_t BufferedReader::getAtnCode(void)
{
    return Utils::getWord(&buffer[4]);
}

uint32_t BufferedReader::dataSizeRest(void)
{
    return dataSizeBytes;
}

void BufferedReader::decreaseDataSize(uint32_t decreaseBy)
{
    dataSizeBytes = (dataSizeBytes >= decreaseBy) ? (dataSizeBytes - decreaseBy) : 0;
}

// pointer to header start, so the header would end up looking like 8 bytes (even if it's really HEADER_BUFFER_SIZE bytes long)
// and the ATN code is at the index 3
uint8_t* BufferedReader::getHeaderPointer(void)
{
    return &buffer[2];
}

// clear buffer after reading valid header
void BufferedReader::clear(void)
{
    gotBytes = 0;               // don't have any bytes anymore
    memset(buffer, 0, HEADER_BUFFER_SIZE);
}

// clear buffer and remaining data size, e.g. when client disconnects
void BufferedReader::reset(void)
{
    clear();
    dataSizeBytes = 0;          // no data left to read

    if(skippedBytes > 0) {      // connection closing while still out of sync? log what we've dropped
        Debug::out(whichLog, LOG_DEBUG, "BufferedReader::reset() - %d bytes skipped without finding valid header", skippedBytes);
        skippedBytes = 0;
    }
}
