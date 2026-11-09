#ifndef __MFM_PERF_H__
#define __MFM_PERF_H__

#include <cstdint>

// Timing test of the track stream on the real hardware (MFM_PERF_TEST in defs.h): after the real refill the DMA IRQ also
// encodes one of the test tracks into a scratch buffer, the worst case STX one too. So the timing is measured with WiFi,
// the host connection, flash access and write capture running, and without touching the real stream.

#define MFM_PERF_KINDS  3               // standard track, worst case STX track, flux in every 2nd cell (most intervals per byte)

void mfmPerfInit(void);                 // core 1, before the DMA IRQ starts
void mfmPerfOnRefill(void);             // DMA IRQ - one v2 refill of one of the test tracks
void mfmPerfRecordRefill(uint32_t us);  // DMA IRQ - how long the real refill took
void mfmPerfLoop(void);                 // core 1 main loop - measures the seek
void mfmPerfReport(void);               // core 0 - results to the debug log

const uint8_t* mfmPerfTrack(int kind);  // the test track

#endif
