// Replacement of ce_pico_hdd_scsi/bridge.cpp (PIO / DMA towards the Atari) - scripted bytes in, recorded bytes out.

#ifndef BRIDGE_FAKE_H
#define BRIDGE_FAKE_H

#include <deque>
#include <vector>
#include <cstdint>

struct FakeBridge {
    uint8_t              selectedId        = 0xff;  // what getSelectionByte() returns, 0xff = this device not selected
    std::deque<bool>     atn;                       // isAtnAsserted() results, false when empty
    std::deque<uint8_t>  fromAtari;                 // bytes PIO_write / DMA_write return, E_TimeOut when empty
    std::vector<uint8_t> dmaToAtari;                // bytes sent with DMA_read
    std::vector<uint8_t> statusToAtari;             // bytes sent with statusAndMsgRead
    std::vector<int>     modes;                     // pioConfig() calls
    int                  currentMode       = 0;
    int                  resetCount        = 0;
    uint32_t             dmaWriteCount     = 0;
    int                  dmaReadFailAfter  = -1;    // DMA_read times out after this many bytes, -1 = never
    bool                 waitForEndFails   = false;
};

extern FakeBridge fakeBridge;

#endif
