// Replacement of ce_pico_hdd_acsi/bridge.cpp, see bridge_fake.h

#include "bridge_fake.h"
#include "bridge.h"

extern uint8_t brStat;

FakeBridge fakeBridge;

static uint8_t nextFromAtari(void)
{
    if (fakeBridge.fromAtari.empty()) {
        brStat = E_TimeOut;
        return 0;
    }

    brStat = E_OK;
    uint8_t b = fakeBridge.fromAtari.front();
    fakeBridge.fromAtari.pop_front();
    return b;
}

void pioConfigAll(void) {}
void pioConfig(int newMode, bool) { fakeBridge.modes.push_back(newMode); }
void resetBridge(void) { fakeBridge.resetCount++; }

uint8_t PIO_gotFirstCmdByte(void) { return !fakeBridge.fromAtari.empty(); }
uint8_t PIO_writeFirst(void) { return nextFromAtari(); }
uint8_t PIO_write(void) { return nextFromAtari(); }

void PIO_read(uint8_t scsiStatusByte) { fakeBridge.statusToAtari.push_back(scsiStatusByte); }

void DMA_write_startWithCount(uint32_t transfersCount) { fakeBridge.dmaWriteCount = transfersCount; }
uint8_t DMA_write(void) { return nextFromAtari(); }

void DMA_read(uint8_t val)
{
    if (fakeBridge.dmaReadFailAfter >= 0 && (int) fakeBridge.dmaToAtari.size() >= fakeBridge.dmaReadFailAfter) {
        brStat = E_TimeOut;
        return;
    }

    brStat = E_OK;
    fakeBridge.dmaToAtari.push_back(val);
}

void DMA_read_waitForEnd(void) { brStat = fakeBridge.waitForEndFails ? E_TimeOut : E_OK; }
