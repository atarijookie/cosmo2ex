#include "defs.h"
#include "keys.h"

Key::Key() : rawState(false), rawChangeTime(0), stableState(false), pressTime(0), longSent(false),
         veryLongSent(false)
{
}

char Key::update(bool pressed, uint32_t now)
{
    if(pressed != rawState) {
        rawState = pressed;
        rawChangeTime = now;
    }

    // raw state stable for long enough and different from the debounced one - it's a press / release
    if(rawState != stableState && now - rawChangeTime >= DEBOUNCE_MS) {
        stableState = rawState;

        if(stableState) {
            pressTime = now;
            longSent = false;
            veryLongSent = false;
        } else if(!longSent) {
            return KEY_EVENT_SHORT;
        }
    }

    if(stableState && !longSent && now - pressTime >= LONG_PRESS_MS) {
        longSent = true;
        return KEY_EVENT_LONG;
    }

    if(stableState && longSent && !veryLongSent && now - pressTime >= VERY_LONG_PRESS_MS) {
        veryLongSent = true;
        return KEY_EVENT_VERY_LONG;
    }

    return KEY_EVENT_NONE;
}

QuadDecoder::QuadDecoder(int stepsPerDetent) : stepsPerDetent(stepsPerDetent), prevState(3), acc(0)
{
}

int QuadDecoder::update(bool a, bool b)
{
    // index: previous AB << 2 | current AB; invalid (skipped) transitions count 0, bounces cancel out
    static const int8_t transitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

    uint8_t state = (a ? 2 : 0) | (b ? 1 : 0);
    acc += transitions[(prevState << 2) | state];
    prevState = state;

    if(acc >= stepsPerDetent) {
        acc -= stepsPerDetent;
        return 1;
    }

    if(acc <= -stepsPerDetent) {
        acc += stepsPerDetent;
        return -1;
    }

    return 0;
}
