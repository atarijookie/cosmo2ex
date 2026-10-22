#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include "client_context.h"

void showRunningStateOnDisplay(void);

void connectToHost(void);

bool sendDataToHost(uint8_t *bfr, uint32_t dataSizeBytes);
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes);

#define SYNC_TAG_FDD    0xc0500fdd

typedef struct {
    struct tcp_pcb *pcb;
    ClientContext *cc;
} TConnection;

size_t conWrite(TConnection* client, const uint8_t *buf, size_t size);
size_t conRead(TConnection* con, uint8_t *buf, size_t size);

bool connectionAvailable(TConnection* client);
bool isConnected(TConnection* client);
size_t connectionCanReadBytes(TConnection* client);
void connect(TConnection* client, ip_addr_t* addr, uint16_t port);
void stop(TConnection* client);

#endif
