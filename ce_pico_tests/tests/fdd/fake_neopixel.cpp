#include "fake_neopixel.h"
#include "neopixel.h"
#include "utils.h"

std::vector<FakeNeopixelFrame> fakeNeopixelFrames;

void neopixelOutputInit(void)
{
    fakeNeopixelFrames.clear();
}

void neopixelOutput(uint32_t grb)
{
    fakeNeopixelFrames.push_back({millis(), grb});
}
