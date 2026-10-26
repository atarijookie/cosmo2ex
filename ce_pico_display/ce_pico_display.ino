// Cosmo2ex FDD front panel - RP2040-Zero with a display, 3 buttons or a rotary encoder.
// Talks to the FDD over UART, see protocol.md. The same messages are accepted on the USB serial too (for testing
// the panel from a PC terminal), and the panel's messages are sent there as well when a terminal is open.

#include <Arduino.h>
#include <hardware/gpio.h>

#include "defs.h"
#include "protocol.h"
#include "keys.h"
#include "screen.h"
#include "settings.h"

static bool hasEncoder;

static LineReceiver rxUart;
static LineReceiver rxUsb;

static Key keyLeft, keyEnter, keyRight;

static QuadDecoder encoder(ENC_STEPS_PER_DETENT);
static volatile int encoderDetents;     // + CW, - CCW, written by the ISR

static void send(const char* msg)
{
    Serial1.print(msg);

    if(Serial) {                        // USB terminal open
        Serial.print(msg);
    }
}

static void sendIdentify(void)
{
    char caps[8];
    char* c = caps;

    if(hasEncoder) {
        *c++ = 'E';
    }
    *c++ = 'S';
    *c++ = 'D';
    *c = 0;

    char msg[48];
    snprintf(msg, sizeof(msg), "I%d,%d,%d,%d,%s,%s\n", PROTOCOL_VERSION, screenLines(), screenCols(), screenCols2(),
             caps, PANEL_FW_VERSION);
    send(msg);
}

static void sendKey(char button, char event)
{
    char msg[5] = {'K', button, event, '\n', 0};
    send(msg);
}

static void handleCommand(const Command& cmd, uint32_t now)
{
    switch(cmd.type) {
        case CMD_IDENTIFY:   sendIdentify();                                                 break;
        case CMD_CLEAR:      screenClear(now);                                               break;
        case CMD_TEXT:       screenSetLine(cmd.line, cmd.justify, cmd.flags, cmd.text, now); break;
        case CMD_BRIGHTNESS: screenSetBrightness(cmd.brightness, now);                       break;
        case CMD_BOOTLOADER: rp2040.rebootToBootloader();                                    break;
    }
}

static void receive(Stream& port, LineReceiver& rx, uint32_t now)
{
    while(port.available()) {
        if(rx.feed(port.read())) {
            Command cmd;
            if(parseCommand(rx.line(), cmd)) {
                handleCommand(cmd, now);
            }
        }
    }
}

static void encoderIsr(void)
{
    int step = encoder.update(digitalRead(PIN_BTN_LEFT), digitalRead(PIN_BTN_RIGHT));
    encoderDetents += ENC_REVERSE ? -step : step;
}

// enter held for VERY_LONG_PRESS_MS - upside down <-> normal, stored
static void flipScreen(uint32_t now)
{
    bool flipped = !settingsFlipped();
    screenSetFlipped(flipped, now);
    screenLoop(now);                    // shown before the flash write, which stops everything for a moment

    settingsSetFlipped(flipped);
    sendIdentify();                     // the FDD sends the whole screen again - in case a line was lost meanwhile
}

static void handleKey(Key& key, int pin, char button, uint32_t now)
{
    char event = key.update(digitalRead(pin) == LOW, now);      // pressed = pulled to GND

    if(event == KEY_EVENT_VERY_LONG) {  // not for the FDD
        if(button == 'E') {
            flipScreen(now);
        }
    } else if(event != KEY_EVENT_NONE) {
        screenActivity(now);
        sendKey(button, event);
    }
}

static void handleEncoder(uint32_t now)
{
    noInterrupts();
    int detents = encoderDetents;
    encoderDetents = 0;
    interrupts();

    for(; detents > 0; detents--) {
        screenActivity(now);
        sendKey('R', KEY_EVENT_SHORT);
    }

    for(; detents < 0; detents++) {
        screenActivity(now);
        sendKey('L', KEY_EVENT_SHORT);
    }
}

void setup(void)
{
    pinMode(PIN_STRAP_ENCODER, INPUT_PULLUP);
    pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
    pinMode(PIN_BTN_ENTER, INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
    delay(1);                           // let the pull-ups settle before reading the strap

    hasEncoder = digitalRead(PIN_STRAP_ENCODER) == LOW;

    Serial.begin(115200);               // USB
    Serial1.setTX(PIN_UART_TX);
    Serial1.setRX(PIN_UART_RX);
    Serial1.begin(UART_BAUD);
    gpio_pull_up(PIN_UART_RX);          // the pads default to pull-down - with no FDD that's a break, not idle

    settingsInit();
    screenInit();
    screenSetFlipped(settingsFlipped(), millis());

    if(hasEncoder) {
        attachInterrupt(digitalPinToInterrupt(PIN_BTN_LEFT), encoderIsr, CHANGE);
        attachInterrupt(digitalPinToInterrupt(PIN_BTN_RIGHT), encoderIsr, CHANGE);
    }

    sendIdentify();                     // tells the FDD we're here, it sends the whole screen then
}

void loop(void)
{
    uint32_t now = millis();

    receive(Serial1, rxUart, now);
    receive(Serial, rxUsb, now);

    if(hasEncoder) {
        handleEncoder(now);
    } else {
        handleKey(keyLeft, PIN_BTN_LEFT, 'L', now);
        handleKey(keyRight, PIN_BTN_RIGHT, 'R', now);
    }
    handleKey(keyEnter, PIN_BTN_ENTER, 'E', now);       // button or encoder push

    screenLoop(now);
}
