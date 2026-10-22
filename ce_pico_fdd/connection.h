#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include <WiFi.h>
#include <string>

void showRunningStateOnDisplay(void);

void connectToHost(void);

bool sendDataToHost(uint8_t *bfr, uint32_t dataSizeBytes);
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes);

#define SYNC_TAG_FDD    0xc0500fdd

typedef WiFiClient TConnection;     // a TCP connection to host (fdd, ikbd)

size_t conWrite(TConnection* con, const uint8_t *buf, size_t size);
size_t conRead(TConnection* con, uint8_t *buf, size_t size);

bool isConnected(TConnection* con);
size_t connectionCanReadBytes(TConnection* con);
void connect(TConnection* con, const IPAddress& addr, uint16_t port);
void stop(TConnection* con);

#endif
