// Host replacement for the arduino-pico core header - just the bits the ce_pico_hdd_* sources use.

#ifndef ARDUINO_FAKE_H
#define ARDUINO_FAKE_H

#include <string>
#include "pico_fake.h"

// minimal Arduino String
class String
{
public:
    String() {}
    String(const char *s) : str(s) {}
    String(const std::string &s) : str(s) {}

    unsigned int length(void) const { return str.length(); }
    const char *c_str(void) const { return str.c_str(); }
    bool operator==(const char *s) const { return str == s; }

private:
    std::string str;
};

// minimal Arduino IPAddress
class IPAddress
{
public:
    IPAddress() : octets{0, 0, 0, 0} {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets{a, b, c, d} {}

    uint8_t operator[](int i) const { return octets[i]; }
    uint8_t &operator[](int i) { return octets[i]; }

    bool operator==(const IPAddress &o) const { return memcmp(octets, o.octets, 4) == 0; }
    bool operator!=(const IPAddress &o) const { return !(*this == o); }

    String toString(void) const
    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
        return String(tmp);
    }

private:
    uint8_t octets[4];
};

#endif
