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
#define PIN_BEEP            14
#define PIN_STRAP_ACTIVE    15      // open = passive piezo (PWM tone), GND = active buzzer (DC on/off)

#define UART_BAUD           115200

// keys
#define DEBOUNCE_MS         20
#define LONG_PRESS_MS       1000
#define ENC_STEPS_PER_DETENT 4      // quadrature transitions per click, some encoders have 2
#define ENC_REVERSE         0       // 1 if CW turn gives left on your encoder

// beeper
#define BEEP_MAX_MS         5000
#define BEEP_DEFAULT_HZ     2700
#define BEEP_MIN_HZ         100
#define BEEP_MAX_HZ         10000

// screen
#define REDRAW_DELAY_MS     20      // redraw this long after the last change, so a whole screen doesn't flicker
#define SCROLL_STEP_MS      40      // scrolled text moves 1 px per step
#define SCROLL_PAUSE_MS     1000    // pause at the start and at the end of the scrolled text
#define DIM_AFTER_MS        (5 * 60 * 1000)     // burn-in: dim after this long without change or key press
#define DIM_CONTRAST        1
#define DEFAULT_CONTRAST    255

#endif
