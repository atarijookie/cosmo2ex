#ifndef __CORE1_MAIN_H__
#define __CORE1_MAIN_H__

#define FILL_NONE   0
#define FILL_LOWER  1
#define FILL_UPPER  2

extern volatile uint8_t fillWhat;

void setupPwmOutput(void);
void setupDmaToPwm(void);

void fillHalfMfmBuffer(void);

struct IPCbuffer;
void storeReceivedTrack(IPCbuffer* bfr);

void writeCaptureSetup(void);       // start write capture: PIO + DMA, write buffers, WGATE IRQ
void handleWriteBuffers(void);      // core1 main loop part: store finished writes, prepare buffers for next ones
void onWriteStart(void);            // from WGATE IRQ
void onWriteEnd(void);              // from WGATE IRQ

void streamOutputEnableSetup(void); // read data output enable follows DRIVE_SEL + MOT_EN from now on, call on core1
void trackRotationSetup(void);      // start the INDEX pulse + track restart alarm IRQ, call on core1

void core1Setup(void);             // core 1 start, from setup1()
void core1Loop(void);              // one pass of the core 1 main loop, from loop1()

#endif
