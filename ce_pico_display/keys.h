#ifndef KEYS_H_
#define KEYS_H_

// button debounce + short / long press, and the rotary encoder decoder - hardware independent, tested on the host

#include <cstdint>

#define KEY_EVENT_NONE      0
#define KEY_EVENT_SHORT     'S'     // released before LONG_PRESS_MS
#define KEY_EVENT_LONG      'H'     // held for LONG_PRESS_MS - sent right then, the release after it sends nothing

class Key
{
public:
    Key();
    char update(bool pressed, uint32_t now);    // call often with the raw state, returns KEY_EVENT_*

private:
    bool     rawState;
    uint32_t rawChangeTime;
    bool     stableState;
    uint32_t pressTime;
    bool     longSent;
};

class QuadDecoder
{
public:
    explicit QuadDecoder(int stepsPerDetent);
    int update(bool a, bool b);     // call on every A / B change, returns +1 / -1 per detent, 0 otherwise

private:
    int     stepsPerDetent;
    uint8_t prevState;
    int     acc;
};

#endif
