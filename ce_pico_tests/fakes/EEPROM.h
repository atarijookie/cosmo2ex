// Host replacement for the arduino-pico EEPROM library - a RAM array the tests can inspect and preset.

#ifndef EEPROM_FAKE_H
#define EEPROM_FAKE_H

#include <cstdint>
#include <cstring>

class FakeEEPROM
{
public:
    static const int CAPACITY = 4096;

    uint8_t bytes[CAPACITY];
    int     size        = 0;
    int     commitCount = 0;

    void reset(void) { memset(bytes, 0xff, sizeof(bytes)); size = 0; commitCount = 0; }    // erased flash reads as 0xff

    void    begin(int sz)               { size = sz; }
    uint8_t read(int addr)              { return bytes[addr]; }
    void    write(int addr, uint8_t v)  { bytes[addr] = v; }
    bool    commit(void)                { commitCount++; return true; }
};

extern FakeEEPROM EEPROM;

#endif
