#include "hub75_frame.h"

static bool pixelAt(const uint8_t* buf, int x, int y)
{
    return buf && (buf[(y / 8) * HUB75_WIDTH + x] & (1 << (y & 7)));
}

void hub75BuildFrame(const uint8_t* buf, uint8_t color, uint32_t onCycles, bool mirrorX, uint32_t* frame)
{
    uint32_t* w = frame;

    for(int row = 0; row < HUB75_SCAN_ROWS; row++) {
        uint32_t address = ((uint32_t) row) << HUB75_ADDR_SHIFT;

        *w++ = HUB75_WIDTH - 1;

        // the first pixel shifted in ends up the farthest from the input - on the last panel of the chain
        for(int i = 0; i < HUB75_WIDTH; i++) {
            int x = mirrorX ? HUB75_WIDTH - 1 - i : i;
            uint32_t pixel = address;

            if(pixelAt(buf, x, row)) {
                pixel |= color;
            }
            if(pixelAt(buf, x, row + HUB75_SCAN_ROWS)) {
                pixel |= color << 3;
            }
            *w++ = pixel;
        }

        *w++ = onCycles - 1;
    }
}

uint32_t hub75OnCycles(uint8_t brightness, uint32_t maxOnCycles)
{
    return 1 + (brightness * maxOnCycles) / 255;
}
