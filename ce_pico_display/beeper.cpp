#include <Arduino.h>

#include "defs.h"
#include "beeper.h"

static bool active;         // active buzzer (own generator) - just on / off, the frequency is ignored
static bool beeping;
static uint32_t beepStart;
static uint32_t beepLength;

static void beeperStop(void)
{
    if(active) {
        digitalWrite(PIN_BEEP, LOW);
    } else {
        noTone(PIN_BEEP);
    }
    beeping = false;
}

void beeperInit(bool activeBuzzer)
{
    active = activeBuzzer;
    pinMode(PIN_BEEP, OUTPUT);
    digitalWrite(PIN_BEEP, LOW);
    beeping = false;
}

void beeperStart(uint16_t ms, uint16_t hz, uint32_t now)
{
    if(beeping) {
        beeperStop();
    }

    if(ms == 0) {
        return;
    }

    if(active) {
        digitalWrite(PIN_BEEP, HIGH);
    } else {
        tone(PIN_BEEP, hz);
    }

    beeping = true;
    beepStart = now;
    beepLength = ms;
}

void beeperLoop(uint32_t now)
{
    if(beeping && now - beepStart >= beepLength) {
        beeperStop();
    }
}
