// core1_main.cpp - read stream refill: the DMA plays the MFM ring buffer out to the PWM in two halves, the DMA IRQ fills
// the half that was just played out. The half must come from where the DMA reads now: when core 1 can't take the IRQs
// for longer than one half (idled for a flash write - EEPROM.commit() on a settings change), several IRQs merge into
// one, and a half chosen by flipping a flag would stay on the wrong one - filling the half being played out, for good.

#include <cstdint>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "core1_main.h"

extern uint16_t mfmBuffer[];                // core1_main.cpp - the ring the DMA plays out, 2 halves
extern int dmaChannel;
void dmaHandlerMfm(void);

namespace {

const uintptr_t HALF_BYTES = 1024 * sizeof(uint16_t);      // MFM_BUFFER_HALF_SIZE items

struct RefillFixture {
    RefillFixture() {
        fakeReset();
        setupDmaToPwm();
    }

    // the DMA is reading at this byte offset in the ring when the IRQ is handled
    uint8_t irqWhileReadingAt(uintptr_t offset) {
        fakeDmaHw.ch[dmaChannel].read_addr = (uintptr_t) mfmBuffer + offset;
        dmaHandlerMfm();
        return fillWhat;
    }
};

}   // namespace

TEST_SUITE("fdd read stream refill") {

TEST_CASE_FIXTURE(RefillFixture, "the half that was played out is filled - the other one than the DMA reads") {
    CHECK(irqWhileReadingAt(HALF_BYTES) == FILL_LOWER);        // lower done, the DMA went on to the upper one
    CHECK(irqWhileReadingAt(0) == FILL_UPPER);                 // upper done, wrapped to the lower one
    CHECK(irqWhileReadingAt(HALF_BYTES + 6) == FILL_LOWER);    // IRQ a bit late
    CHECK(irqWhileReadingAt(10) == FILL_UPPER);
}

TEST_CASE_FIXTURE(RefillFixture, "IRQs merged while core 1 was idled - still the half the DMA doesn't read") {
    CHECK(irqWhileReadingAt(HALF_BYTES) == FILL_LOWER);
    // core 1 idled for a flash write: the DMA played lower, upper, lower - the 3 IRQs come as 1, it reads upper again
    CHECK(irqWhileReadingAt(HALF_BYTES + 100) == FILL_LOWER);
    CHECK(irqWhileReadingAt(0) == FILL_UPPER);                 // and it goes on normally
}

}   // TEST_SUITE
