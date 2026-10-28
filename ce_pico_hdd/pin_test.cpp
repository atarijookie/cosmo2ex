// TEMPORARY - hardware test of the bus pins of the ACSI variant, for finding shorts / open pins on the board.
// Pins are set up like in normal usage (core1_setup() + ikbdInit()), but driven by SIO instead of PIO.
// Each output goes H for PIN_TEST_HIGH_MS while all the other outputs are L, so a pin shorted to it shows H too
// (with all pins H at once, shorted pins would look fine). Every change and the levels of all inputs go to the debug
// console, so you can measure at the right time. Loops forever.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "pin_test.h"

#define PIN_TEST_HIGH_MS    3000
#define PIN_TEST_INPUT_READS   3        // how many times inputs are read (1 s apart) in the input part of the test

typedef struct {
    int         pin;
    const char *name;
} TestPin;

// outputs in normal usage - D0..D7 only while DATA_DIR is H (sending to ST)
static const TestPin acsiDataPins[] = {
    {PIN_D0, "D0"}, {PIN_D1, "D1"}, {PIN_D2, "D2"}, {PIN_D3, "D3"},
    {PIN_D4, "D4"}, {PIN_D5, "D5"}, {PIN_D6, "D6"}, {PIN_D7, "D7"}
};

static const TestPin acsiOutPins[] = {
    {PIN_INT, "INT"}, {PIN_DRQ, "DRQ"}, {PIN_NEOPIXEL, "NEOPIXEL"}, {PIN_KEYB_TX, "KEYB_TX"}
};

// inputs in normal usage (D0..D7 too, while DATA_DIR is L)
static const TestPin acsiInPins[] = {
    {PIN_A1, "A1"}, {PIN_CS, "CS"}, {PIN_ACK, "ACK"}, {PIN_RESET, "RESET"},
    {PIN_KEYB_RX, "KEYB_RX"}, {PIN_KEYB_TX_ORIG, "KEYB_TX_ORIG"}
};

#define COUNT(X)    ((int) (sizeof(X) / sizeof(X[0])))

static void pinAsOutput(int pin, int level)
{
    gpio_init(pin);                     // SIO function, input, output latch 0
    gpio_put(pin, level);               // level first, so it doesn't glitch when switched to output
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
}

static void pinAsInput(int pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);            // like pinMode(INPUT) in normal usage
}

static void dataPinsAsOutputs(bool output)
{
    for(int i=0; i<COUNT(acsiDataPins); i++) {
        if(output) {
            pinAsOutput(acsiDataPins[i].pin, 0);
        } else {
            pinAsInput(acsiDataPins[i].pin);
        }
    }
}

// DATA_DIR H = from Pico to ST, L = from ST to Pico; the data pins are switched so they never fight the buffer
static void setDataDirection(bool sendNotRecv)
{
    if(sendNotRecv) {
        BIT_SET(PIN_DATA_DIR);          // like setDataDirection() in bridge_acsi.cpp: DATA_DIR H before outputs
        dataPinsAsOutputs(true);
    } else {
        dataPinsAsOutputs(false);       // inputs before DATA_DIR L
        BIT_CLR(PIN_DATA_DIR);
    }
}

static void appendPinLevels(char *bfr, int bfrSize, const TestPin *pins, int count)
{
    for(int i=0; i<count; i++) {
        int len = strlen(bfr);
        snprintf(bfr + len, bfrSize - len, " %s=%c", pins[i].name, BIT_IS_H(pins[i].pin) ? 'H' : 'L');
    }
}

static void showInputs(const char *prefix, bool withDataPins)
{
    char bfr[256];
    snprintf(bfr, sizeof(bfr), "%-24s| in:", prefix);
    appendPinLevels(bfr, sizeof(bfr), acsiInPins, COUNT(acsiInPins));

    if(withDataPins) {
        appendPinLevels(bfr, sizeof(bfr), acsiDataPins, COUNT(acsiDataPins));
    }

    debug("%s\n", bfr);
}

// one output H for PIN_TEST_HIGH_MS, then back to L - with the inputs shown after each change
static void pulsePin(const TestPin *p)
{
    char bfr[48];

    BIT_SET(p->pin);
    snprintf(bfr, sizeof(bfr), "GP%d %s -> H", p->pin, p->name);
    showInputs(bfr, false);

    sleep_ms(PIN_TEST_HIGH_MS);

    BIT_CLR(p->pin);
    snprintf(bfr, sizeof(bfr), "GP%d %s -> L", p->pin, p->name);
    showInputs(bfr, false);
}

void pinTestAcsi(void)
{
    debug("\n\n!!! PIN TEST - ACSI variant, never returns, comment out pinTestAcsi() call for normal usage !!!\n");

    // pins like in normal usage, but all outputs L, so the one going H stands out
    for(int i=0; i<COUNT(acsiInPins); i++) {
        pinAsInput(acsiInPins[i].pin);
    }

    for(int i=0; i<COUNT(acsiOutPins); i++) {
        pinAsOutput(acsiOutPins[i].pin, 0);
    }

    pinAsOutput(PIN_DATA_DIR, 0);
    dataPinsAsOutputs(false);

    for(int loop=1; ; loop++) {
        debug("\n=== pin test loop %d ===\n", loop);

        // data pins towards ST - DATA_DIR H, D0..D7 outputs
        debug("--- data pins, DATA_DIR = H (Pico -> ST)\n");
        setDataDirection(true);

        for(int i=0; i<COUNT(acsiDataPins); i++) {
            pulsePin(&acsiDataPins[i]);
        }

        // the other outputs
        debug("--- other outputs (NEOPIXEL reaches the LED only while INT is H)\n");
        for(int i=0; i<COUNT(acsiOutPins); i++) {
            pulsePin(&acsiOutPins[i]);
        }

        // DATA_DIR itself - data pins as inputs, so they don't fight the buffer in any direction
        debug("--- DATA_DIR, data pins are inputs now\n");
        dataPinsAsOutputs(false);

        const TestPin dataDir = {PIN_DATA_DIR, "DATA_DIR"};
        pulsePin(&dataDir);

        // data pins from ST - DATA_DIR L, D0..D7 inputs
        debug("--- inputs, DATA_DIR = L (ST -> Pico)\n");
        setDataDirection(false);

        for(int i=0; i<PIN_TEST_INPUT_READS; i++) {
            showInputs("reading inputs", true);
            sleep_ms(1000);
        }
    }
}
