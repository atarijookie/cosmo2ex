// Fake of the neopixel output (ce_pico_fdd/neopixel_pio.cpp) - records the frames.

#ifndef FAKE_NEOPIXEL_H
#define FAKE_NEOPIXEL_H

#include <vector>
#include <cstdint>

struct FakeNeopixelFrame {
    uint32_t timeMs;
    uint32_t grb;
};

extern std::vector<FakeNeopixelFrame> fakeNeopixelFrames;

#endif
