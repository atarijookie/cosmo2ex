// keys.cpp of ce_pico_display - button debounce, short / long press, rotary encoder decoding

#include <string>

#include "doctest.h"

#include "defs.h"
#include "keys.h"

namespace {

// runs the key with the given raw state from 'from' to 'to' (ms, 1 ms steps), returns the events
std::string run(Key& key, bool pressed, uint32_t from, uint32_t to)
{
    std::string events;
    for (uint32_t t = from; t < to; t++) {
        char e = key.update(pressed, t);
        if (e != KEY_EVENT_NONE) {
            events += e;
        }
    }
    return events;
}

// one full quadrature cycle (4 transitions) in the given direction, from the rest state (both high)
int turn(QuadDecoder& enc, bool cw)
{
    static const bool seqCw[4][2]  = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    static const bool seqCcw[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};

    int sum = 0;
    for (int i = 0; i < 4; i++) {
        const bool* s = cw ? seqCw[i] : seqCcw[i];
        sum += enc.update(s[0], s[1]);
    }
    return sum;
}

}

TEST_CASE("short press is sent on release")
{
    Key key;
    CHECK(run(key, false, 0, 100) == "");
    CHECK(run(key, true, 100, 400) == "");          // pressed, nothing yet
    CHECK(run(key, false, 400, 500) == "S");
}

TEST_CASE("long press is sent when the threshold is reached, the release sends nothing")
{
    Key key;
    run(key, false, 0, 100);

    std::string events;
    uint32_t longAt = 0;
    for (uint32_t t = 100; t < 100 + LONG_PRESS_MS + 100; t++) {
        if (key.update(true, t) == KEY_EVENT_LONG) {
            events += 'H';
            longAt = t;
        }
    }
    CHECK(events == "H");
    CHECK(longAt == 100 + DEBOUNCE_MS + LONG_PRESS_MS);     // counted from the debounced press

    CHECK(run(key, false, 1300, 1400) == "");
    CHECK(run(key, true, 1400, 1500) == "");                // the next press works normally again
    CHECK(run(key, false, 1500, 1600) == "S");
}

TEST_CASE("very long press - held on after the long press, sent once at the threshold, the release sends nothing")
{
    Key key;
    run(key, false, 0, 100);

    std::string events;
    uint32_t veryLongAt = 0;
    for (uint32_t t = 100; t < 100 + VERY_LONG_PRESS_MS + 1000; t++) {
        char e = key.update(true, t);
        if (e != KEY_EVENT_NONE) {
            events += e;
        }
        if (e == KEY_EVENT_VERY_LONG) {
            veryLongAt = t;
        }
    }
    CHECK(events == "HV");
    CHECK(veryLongAt == 100 + DEBOUNCE_MS + VERY_LONG_PRESS_MS);

    uint32_t end = 100 + VERY_LONG_PRESS_MS + 1000;
    CHECK(run(key, false, end, end + 100) == "");
}

TEST_CASE("released before the very long press - just the long press")
{
    Key key;
    run(key, false, 0, 100);
    CHECK(run(key, true, 100, 100 + VERY_LONG_PRESS_MS / 2) == "H");
    CHECK(run(key, false, 100 + VERY_LONG_PRESS_MS / 2, VERY_LONG_PRESS_MS * 2) == "");
}

TEST_CASE("very long press - again on the next press")
{
    Key key;
    run(key, false, 0, 100);
    CHECK(run(key, true, 100, 100 + VERY_LONG_PRESS_MS + 100) == "HV");
    CHECK(run(key, false, 100 + VERY_LONG_PRESS_MS + 100, 20000) == "");
    CHECK(run(key, true, 20000, 20000 + VERY_LONG_PRESS_MS + 100) == "HV");
}

TEST_CASE("bounces shorter than the debounce time are ignored")
{
    Key key;
    run(key, false, 0, 100);

    std::string events;
    for (uint32_t t = 100; t < 300; t++) {
        bool pressed = (t / 5) % 2;                 // toggles every 5 ms
        char e = key.update(pressed, t);
        if (e != KEY_EVENT_NONE) {
            events += e;
        }
    }
    CHECK(events == "");
    CHECK(run(key, false, 300, 400) == "");
}

TEST_CASE("encoder - one event per detent in each direction")
{
    QuadDecoder enc(4);
    CHECK(turn(enc, true) == 1);
    CHECK(turn(enc, true) == 1);
    CHECK(turn(enc, false) == -1);
}

TEST_CASE("encoder - bouncing on one transition doesn't count")
{
    QuadDecoder enc(4);
    int sum = 0;
    sum += enc.update(0, 1);
    sum += enc.update(1, 1);        // bounce back
    sum += enc.update(0, 1);
    sum += enc.update(0, 0);
    sum += enc.update(1, 0);
    sum += enc.update(1, 1);
    CHECK(sum == 1);
}

TEST_CASE("encoder - 2 steps per detent")
{
    QuadDecoder enc(2);
    CHECK(turn(enc, true) == 2);
}
