// Replacement of ce_pico_hdd_acsi/bridge.cpp (PIO / DMA towards the Atari) - scripted bytes in, recorded bytes out.

#ifndef BRIDGE_FAKE_H
#define BRIDGE_FAKE_H

#include <deque>
#include <vector>
#include <cstdint>

struct FakeBridge {
    std::deque<uint8_t>  fromAtari;             // bytes PIO_writeFirst / PIO_write / DMA_write return, E_TimeOut when empty
    std::vector<uint8_t> dmaToAtari;            // bytes sent with DMA_read
    std::vector<uint8_t> statusToAtari;         // bytes sent with PIO_read
    std::vector<int>     modes;                 // pioConfig() calls
    int                  resetCount        = 0;
    uint32_t             dmaWriteCount     = 0; // last DMA_write_startWithCount()
    int                  dmaReadFailAfter  = -1;    // DMA_read times out after this many bytes, -1 = never
    bool                 waitForEndFails   = false;
};

extern FakeBridge fakeBridge;

#endif
