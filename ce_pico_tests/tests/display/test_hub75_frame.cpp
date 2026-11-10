// hub75_frame.cpp of ce_pico_display - the u8g2 buffer (SSD1306 layout) to the word stream of the hub75 PIO program

#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest.h"

#include "hub75_frame.h"

namespace {

#define WHITE   (HUB75_RED | HUB75_GREEN | HUB75_BLUE)

struct Frame {
    uint8_t buf[HUB75_WIDTH * HUB75_HEIGHT / 8];
    std::vector<uint32_t> words;

    Frame() : words(HUB75_FRAME_WORDS, 0xdeadbeef) {
        memset(buf, 0, sizeof(buf));
    }

    void set(int x, int y) {
        buf[(y / 8) * HUB75_WIDTH + x] |= 1 << (y & 7);
    }

    void build(uint8_t color = WHITE, uint32_t onCycles = 100, bool mirror = false) {
        hub75BuildFrame(buf, color, onCycles, mirror, words.data());
    }

    // the pixel word shifted in as the i-th of the scan row
    uint32_t pixel(int row, int i) {
        return words[row * HUB75_ROW_WORDS + 1 + i];
    }
};

}

TEST_CASE("hub75 frame - every row: pixel count - 1, the pixels with the row address, the on time - 1")
{
    Frame f;
    f.build(WHITE, 100);

    for (int row = 0; row < HUB75_SCAN_ROWS; row++) {
        CHECK(f.words[row * HUB75_ROW_WORDS] == HUB75_WIDTH - 1);
        CHECK(f.words[row * HUB75_ROW_WORDS + HUB75_ROW_WORDS - 1] == 99);
        for (int i = 0; i < HUB75_WIDTH; i++) {
            CHECK(f.pixel(row, i) == ((uint32_t) row << HUB75_ADDR_SHIFT));     // dark
        }
    }
}

TEST_CASE("hub75 frame - top half on R1 G1 B1, bottom half on R2 G2 B2 of the same scan row")
{
    Frame f;
    f.set(5, 3);            // top half, scan row 3
    f.set(6, 3 + 16);       // bottom half, scan row 3 too
    f.set(127, 31);         // last pixel
    f.build(HUB75_RED);

    uint32_t addr3 = 3u << HUB75_ADDR_SHIFT;
    CHECK(f.pixel(3, 5) == (addr3 | HUB75_RED));
    CHECK(f.pixel(3, 6) == (addr3 | (HUB75_RED << 3)));
    CHECK(f.pixel(3, 7) == addr3);
    CHECK(f.pixel(15, 127) == ((15u << HUB75_ADDR_SHIFT) | (HUB75_RED << 3)));
    CHECK(f.pixel(4, 5) == (4u << HUB75_ADDR_SHIFT));
}

TEST_CASE("hub75 frame - the color of the lit pixels")
{
    Frame f;
    f.set(0, 0);
    f.set(0, 16);
    f.build(HUB75_GREEN | HUB75_BLUE);
    CHECK(f.pixel(0, 0) == 0b110110);
}

TEST_CASE("hub75 frame - mirrored: x = 127 shifted in first")
{
    Frame f;
    f.set(0, 0);
    f.build(WHITE, 100, true);
    CHECK(f.pixel(0, 127) == WHITE);
    CHECK(f.pixel(0, 0) == 0);
}

TEST_CASE("hub75 frame - no buffer is all dark")
{
    std::vector<uint32_t> words(HUB75_FRAME_WORDS);
    hub75BuildFrame(nullptr, WHITE, 10, false, words.data());
    CHECK(words[1] == 0);
    CHECK(words[HUB75_ROW_WORDS + 1] == (1u << HUB75_ADDR_SHIFT));
}

TEST_CASE("hub75 on time - from 1 cycle at brightness 0 to max + 1 at 255, growing")
{
    CHECK(hub75OnCycles(0, 2048) == 1);
    CHECK(hub75OnCycles(255, 2048) == 2049);

    uint32_t prev = 0;
    for (int b = 0; b <= 255; b++) {
        uint32_t c = hub75OnCycles(b, 2048);
        CHECK(c >= prev);
        prev = c;
    }
}
