#ifndef __IPC_H__
#define __IPC_H__

#include <cstdint>

enum IpcCommand {
  CMD_STORE_TRACK,
  CMD_SEND_WRITTEN_SECTOR
};

struct __attribute__((packed)) IPCbuffer
{
    uint8_t index;
    volatile bool free;     // set only via ipcGetFreeBuffer() / ipcBufferRelease() - the other core reads it
    IpcCommand command;
    uint32_t length;

    uint8_t trackNo;
    uint8_t sideNo;
    uint8_t sectorNo;

    uint32_t maxDataSize;
    uint8_t *data;
};

void ipcInit(void);
IPCbuffer* ipcGetFreeBuffer(int coreNo, uint32_t maxWaitMs);
IPCbuffer* ipcGetBufferFromFifo(int coreNo);
void ipcPutBufferToFifo(int coreNo, uint8_t bfrIndex);
void ipcSetBufferAndPutToFifo(IPCbuffer* bfr, int coreNo, IpcCommand command, uint32_t lengthToStore);
void ipcBufferRelease(IPCbuffer* bfr);      // done with the buffer and its data - it can be taken again (also by the other core)

#define IPC_BUFFER_LARGE_COUNT    10
#define IPC_BUFFER_SMALL_COUNT    16

#endif /* __IPC_H__ */
