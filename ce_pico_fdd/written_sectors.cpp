#include <cstring>

#include "defs.h"
#include "connection.h"
#include "ipc.h"
#include "written_sectors.h"

// Written sectors received from core1, in the order they were written, not sent to host yet.
// They stay here until they were sent, so a write done while not connected reaches the host after reconnect.
static IPCbuffer* pendingWrites[IPC_BUFFER_SMALL_COUNT];
static int pendingWritesCount = 0;

void writtenSectorsReset(void)
{
    pendingWritesCount = 0;
}

void writtenSectorsHandle(void)
{
    // take all new written sectors from core1, keep them in order
    while(pendingWritesCount < IPC_BUFFER_SMALL_COUNT) {
        IPCbuffer* bfr = ipcGetBufferFromFifo(0);

        if(!bfr) {                                      // nothing more from core1
            break;
        }

        if(bfr->command != CMD_SEND_WRITTEN_SECTOR) {   // nothing else is expected here, just free it
            ipcBufferRelease(bfr);
            continue;
        }

        pendingWrites[pendingWritesCount++] = bfr;
    }

    // send them to host, oldest first - on failure (not connected) keep it and the rest for later
    while(pendingWritesCount > 0) {
        IPCbuffer* bfr = pendingWrites[0];

        if(!sendHeaderAndDataToHost(bfr->data, bfr->length - TX_HEADER_SIZE)) {
            break;
        }

        ipcBufferRelease(bfr);                          // sent, core1 can use the buffer again
        pendingWritesCount--;
        memmove(pendingWrites, pendingWrites + 1, pendingWritesCount * sizeof(pendingWrites[0]));
    }
}

bool writtenSectorsPending(void)
{
    return pendingWritesCount > 0;
}
