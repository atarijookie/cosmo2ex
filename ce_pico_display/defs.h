#ifndef DEFS_H_
#define DEFS_H_

#include <cstdint>

#define PANEL_FW_VERSION    "0.1"
#define PROTOCOL_VERSION    1

// RP2040-Zero pins, see protocol.md
#define PIN_UART_TX         0       // -> FDD
#define PIN_UART_RX         1       // <- FDD
#define PIN_SDA             4       // display
#define PIN_SCL             5
#define PIN_BTN_LEFT        26      // also encoder A
#define PIN_BTN_ENTER       27      // also encoder push
#define PIN_BTN_RIGHT       28      // also encoder B
#define PIN_STRAP_ENCODER   29      // open = 3 buttons, GND = rotary encoder

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

#endif
