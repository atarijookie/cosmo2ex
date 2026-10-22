// Test-side control of the fake platform for ce_pico_fdd (clock, gpio levels, PSRAM content, IRQ handlers, debug output).

#ifndef FAKES_FDD_H
#define FAKES_FDD_H

#include <string>
#include <vector>
#include <functional>
#include "pico_fdd_fake.h"

#define FAKE_GPIO_COUNT     48
#define FAKE_PSRAM_SIZE     (8 * 1024 * 1024)

extern uint32_t             fakeNowMs;                      // current fake time in ms
extern uint32_t             fakeMillisStep;                 // added to fakeNowMs on every time read, so busy-wait loops can time out
extern std::function<void()> fakeOnTimeRead;                // called on every time read (not nested) - lets a test act during a busy-wait
extern bool                 fakeIrqsDisabled;               // between save_and_disable_interrupts() and restore_interrupts()
extern uint32_t             fakeIrqsOffMaxMs;               // longest time interrupts were disabled
extern bool                 fakeGpioLevel[FAKE_GPIO_COUNT]; // what gpio_get() returns, gpio_put() sets it
extern std::vector<uint8_t> fakePsram;                      // content of the emulated SPI PSRAM chip (read 0x03, write 0x02, read ID 0x9f)
extern std::string          fakeDebugOut;                   // everything sent to the debug uart

extern int     fakeDmaLastClaimed;                          // channel returned by the last dma_claim_unused_channel()
extern uint8_t fakeIrqPriority[64];                         // set by irq_set_priority(), PICO_DEFAULT_IRQ_PRIORITY otherwise

// One DMA transfer of 'value' on 'channel' to its write address (32-bit transfers only). Returns true if this transfer
// completed a block of transfer_count transfers - then the channel's IRQ status is set until acknowledged
// (like the hardware, several completed blocks before the acknowledge are just one pending status).
bool fakeDmaTransfer(uint channel, uint32_t value);

// Change the input level of a pin. If that makes an edge whose IRQ is enabled, the GPIO callback is called (unless
// interrupts are disabled - then the edge is lost, the tests don't need it delivered later).
void fakeGpioSet(uint gpio, bool level);
// GPIO IRQ with the given events, as if they all happened before the IRQ got handled (pin level is not changed)
void fakeGpioIrq(uint gpio, uint32_t events);

// Call the callbacks of all alarms whose time has come (in time order), reschedule the ones that ask for it like the sdk:
// negative return - that many us after the time the alarm was due, positive - after now. Not while interrupts are
// disabled, and not nested in an alarm callback. Called by the fake clock itself, tests don't need to.
void fakeRunDueAlarms(void);

void fakeFireIrq(uint num);     // call the handler registered by irq_set_exclusive_handler() for this irq number
void fakeReset(void);           // resets everything except the clock

#endif
