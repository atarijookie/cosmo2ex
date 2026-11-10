#ifndef DEFS_H_
#define DEFS_H_

#include <cstdint>

#define PANEL_FW_VERSION    "1.0"
#define PROTOCOL_VERSION    1

// RP2040-Zero pins, see protocol.md
#define PIN_UART_TX         0       // -> FDD
#define PIN_UART_RX         1       // <- FDD
#define PIN_SDA             4       // OLED - on the HUB75 board GP4 is B1 (no pull-up there tells the boards apart)
#define PIN_SCL             5
#define PIN_HUB75_DATA      2       // HUB75 board: GP2-7 R1 G1 B1 R2 G2 B2, GP8-12 address A-E (E not connected)
#define PIN_HUB75_CLK       13      // GP13-15 CLK LAT OE - consecutive, PIO side-set
#define PIN_HUB75_OE        15
#define PIN_BTN_LEFT        26      // also encoder A
#define PIN_BTN_ENTER       27      // also encoder push
#define PIN_BTN_RIGHT       28      // also encoder B
#define PIN_STRAP_ENCODER   29      // open = 3 buttons, GND = rotary encoder
#define PIN_NEOPIXEL        16      // WS2812 on the RP2040-Zero board

#define NEOPIXEL_RGB        0       // 0 = GRB (WS2812 standard), 1 = RGB - if orange / green show swapped on your board

#define UART_BAUD           115200

// keys
#define DEBOUNCE_MS         20
#define LONG_PRESS_MS       1000
#define VERY_LONG_PRESS_MS  10000   // enter held this long - flips the screen (the long press was sent at 1 s already)
#define ENC_STEPS_PER_DETENT 4      // quadrature transitions per click, some encoders have 2
#define ENC_REVERSE         0       // 1 if CW turn gives left on your encoder

// screen
#define REDRAW_DELAY_MS     20      // redraw this long after the last change, so a whole screen doesn't flicker
#define SCROLL_STEP_MS      40      // scrolled text moves 1 px per step
#define SCROLL_PAUSE_MS     1000    // pause at the start and at the end of the scrolled text
#define DIM_AFTER_MS        (5 * 60 * 1000)     // burn-in: dim after this long without change or key press
#define DIM_CONTRAST        1
#define DEFAULT_CONTRAST    255
#define DEFAULT_FLIPPED     1       // upside down (U8G2_R2) until changed - the way the encoder panel is mounted
#define DEFAULT_FLIPPED_HUB75 0     // the LED panels - normal

// HUB75 LED panels (2x 64x32 chained, see hub75.pio)
#define HUB75_COLOR         (HUB75_RED | HUB75_GREEN | HUB75_BLUE)      // of the lit pixels, hub75_frame.h
#define HUB75_MIRROR_X      0       // 1 if the text comes out mirrored (if the halves are swapped, swap the panels)
#define HUB75_CLKDIV        4.0f    // PIO clock 133 / 4 MHz - CLK ~11 MHz (3 cycles per pixel)
#define HUB75_MAX_ON_CYCLES 2048    // each row lit up to ~62 us at brightness 255 - refresh ~850 Hz

#endif
