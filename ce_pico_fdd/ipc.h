#ifndef __IPC_H__
#define __IPC_H__

#include <cstdint>

enum IpcCommand {
  CMD_STORE_TRACK,
  CMD_SEND_WRITTEN_SECTOR,
  CMD_STORE_BUILTIN_TRACK,      // track of the built-in floppy, the same form as CMD_STORE_TRACK - see builtin_image.h
  CMD_SHOW_REAL_IMAGE,          // stream the real image from now on, not the built-in floppy
  CMD_SHOW_BUILTIN_IMAGE        // stream the built-in floppy again (WiFi setup started)
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
