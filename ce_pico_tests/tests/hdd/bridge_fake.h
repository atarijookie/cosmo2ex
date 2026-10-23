// Replacement of ce_pico_hdd/bridge*.cpp (PIO / DMA towards the Atari) - scripted bytes in, recorded bytes out.
// Has the fields of both tests/acsi and tests/scsi fakes, as the merged firmware runs the test cases of both.

#ifndef BRIDGE_FAKE_H
#define BRIDGE_FAKE_H

#include <deque>
#include <vector>
#include <cstdint>

struct FakeBridge {
    uint8_t              selectedId        = 0xff;  // SCSI: what getSelectionByte() returns, 0xff = this device not selected
    std::deque<bool>     atn;                       // SCSI: isAtnAsserted() results, false when empty
    std::deque<uint8_t>  fromAtari;                 // bytes PIO_writeFirst / PIO_write / DMA_write return, E_TimeOut when empty
    std::vector<uint8_t> dmaToAtari;                // bytes sent with DMA_read
    std::vector<uint8_t> statusToAtari;             // bytes sent with statusAndMsgRead
    std::vector<int>     modes;                     // pioConfig() calls
    int                  currentMode       = 0;
    int                  resetCount        = 0;
    uint32_t             dmaWriteCount     = 0;     // last DMA_write_startWithCount()
    int                  dmaReadFailAfter  = -1;    // DMA_read times out after this many bytes, -1 = never
    bool                 waitForEndFails   = false;
};

extern FakeBridge fakeBridge;

#endif
