// Shared by all the test files: access to the firmware globals / functions that are not in the firmware headers,
// and a fixture that brings the firmware + fakes into a known state before each test case.

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <vector>
#include <cstdint>

#include "doctest.h"
#include "fakes.h"
#include "EEPROM.h"
#include "Ethernet.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "connection.h"
#include "command_handling.h"

// utils.cpp
extern volatile uint8_t timerRunning;
extern mutex_t debugMutex;
#ifdef LOG_LED
extern queue_t fifoDebug;
#endif

// ipc.cpp
extern IPCbuffer buffersForCore0[IPC_BUFFER_COUNT];
extern IPCbuffer buffersForCore1[IPC_BUFFER_COUNT];
extern queue_t fifoToCore0;
extern queue_t fifoToCore1;

// connection.cpp
extern EthernetUDP    udp;
extern bool           udpInitialized;
extern EthernetClient clientHdd;
extern EthernetClient clientFdd;
extern EthernetClient clientIkbd;
extern uint8_t        hostIp[4];
extern String         hostIpString;
extern uint16_t       hostPortHdd;
extern uint16_t       hostPortFdd;
extern uint16_t       hostPortIkbd;
extern volatile bool  connected;
extern THeader        hddHeader;
extern bool           hddHeaderPending;
extern uint8_t        connectFailCount;

void ceDiscoverySend(void);
void ceDiscoveryReceive(void);
void connectToCEhost(void);
bool getIncommingHeader(EthernetClient* client, uint32_t expectedSyncTag, THeader* header);
void handleAcsiConfig(uint32_t len);
void dhcpMaintain(void);

// command_handling.cpp
extern uint8_t cmd[];          // 16 B in acsi, 17 B in scsi
extern uint8_t cmdLen;
extern uint8_t brStat;

//--------------------------------------------------------

// Test cases describing correct behaviour that the firmware does not have yet. Skipped by default to keep the
// suite green, they run (and fail) with --no-skip, see 'make test-known-bugs'. Remove the decorator once fixed.
#define KNOWN_BUG(msg)  doctest::description("KNOWN BUG: " msg) * doctest::skip()

// one message taken out of the IPC fifo of a core
struct IpcMsg {
    uint8_t              command;
    uint32_t             length;
    std::vector<uint8_t> data;      // the first min(length, IPC_BUFFER_SIZE) bytes of the buffer
};

// takes all buffers out of the fifo for coreNo and frees them (like the core main loops do)
inline std::vector<IpcMsg> drainIpc(int coreNo)
{
    std::vector<IpcMsg> msgs;

    while (IPCbuffer *b = ipcGetBufferFromFifo(coreNo)) {
        uint32_t n = MIN(b->length, (uint32_t) IPC_BUFFER_SIZE);
        msgs.push_back(IpcMsg{ b->command, b->length, std::vector<uint8_t>(b->data, b->data + n) });
        ipcReleaseBuffer(b);
    }

    return msgs;
}

// big-endian header as sent by the CE host: sync tag, cmd code, length
inline std::vector<uint8_t> hostHeader(uint16_t cmdCode, uint32_t len)
{
    return { 0xc0, 0x50, 0xd1, 0xc5, (uint8_t)(cmdCode >> 8), (uint8_t) cmdCode,
             (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t) len };
}

inline void resetFirmwareState(void)
{
    static bool debugInitDone = false;

    timeoutClear();
    fakeReset();
    fakeAdvanceMillis(10000);       // well past any throttling done in the code under test

    if (!debugInitDone) {           // initializes mutex + debug fifo, only once as the fifo is never freed
        debugInit();
        debugInitDone = true;
    }

#ifdef LOG_LED
    debugFromQueue();               // drop whatever a previous test left there
#endif
    fakeDebugOut.clear();

    if (fifoToCore0.data) queue_free(&fifoToCore0);
    if (fifoToCore1.data) queue_free(&fifoToCore1);
    ipcInit();

    memset(&settings, 0, sizeof(settings));
    settings.magic      = SETTINGS_MAGIC;
    settings.enabledIDs = 0x01;

    udp.reset();
    udpInitialized = false;
    clientHdd.reset();
    clientFdd.reset();
    clientIkbd.reset();
    memset(hostIp, 0, sizeof(hostIp));
    hostIpString = String();
    hostPortHdd  = hostPortFdd = hostPortIkbd = 0;
    connected    = false;
    memset(&hddHeader, 0, sizeof(hddHeader));
    hddHeaderPending = false;
    connectFailCount = 0;

    memset(cmd, 0, 16);
    cmdLen = 0;
}

struct FirmwareFixture {
    FirmwareFixture() { resetFirmwareState(); }
};

#endif
