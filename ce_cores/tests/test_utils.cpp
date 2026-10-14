/*
    Tests of small helpers from misc/utils.cpp
*/
#include <string>

#include "doctest.h"

#include "misc/utils.h"

static std::string tz(float utcOffset)
{
    char str[64];
    Utils::createTimezoneStringFromOffset(utcOffset, str);
    return std::string(str);
}

TEST_CASE("createTimezoneStringFromOffset - TZ sign is opposite of UTC offset")
{
    CHECK(tz(0.0f)   == "UTC");
    CHECK(tz(1.0f)   == "UTC-01");      // Berlin, east of Prime Meridian
    CHECK(tz(-5.0f)  == "UTC+05");      // New York, west of Prime Meridian
    CHECK(tz(12.0f)  == "UTC-12");
    CHECK(tz(-12.0f) == "UTC+12");
}

TEST_CASE("createTimezoneStringFromOffset - offsets with minutes")
{
    CHECK(tz(5.5f)   == "UTC-05:30");   // India
    CHECK(tz(5.75f)  == "UTC-05:45");   // Nepal
    CHECK(tz(-3.5f)  == "UTC+03:30");   // Newfoundland
    CHECK(tz(9.5f)   == "UTC-09:30");
}

TEST_CASE("createTimezoneStringFromOffset - offsets smaller than one hour are not lost")
{
    CHECK(tz(0.5f)   == "UTC-00:30");
    CHECK(tz(-0.5f)  == "UTC+00:30");
    CHECK(tz(0.25f)  == "UTC-00:15");
}

TEST_CASE("createTimezoneStringFromOffset - float imprecision is rounded to whole minutes")
{
    CHECK(tz(5.7499f) == "UTC-05:45");
    CHECK(tz(1.0001f) == "UTC-01");
    CHECK(tz(0.001f)  == "UTC");
}

//-------------------------------------------------------------------------------------------------
TEST_CASE("decodeHexEscapes decodes \\xNN from /dev/disk/by-label names")
{
    CHECK(Utils::decodeHexEscapes("NoEscapes") == "NoEscapes");
    CHECK(Utils::decodeHexEscapes("") == "");
    CHECK(Utils::decodeHexEscapes("My\\x20Disk") == "My Disk");
    CHECK(Utils::decodeHexEscapes("a\\x20b\\x20c") == "a b c");
    CHECK(Utils::decodeHexEscapes("\\x41\\x42") == "AB");
    CHECK(Utils::decodeHexEscapes("lower\\x2fcase") == "lower/case");
}

TEST_CASE("decodeHexEscapes keeps invalid or incomplete escapes")
{
    CHECK(Utils::decodeHexEscapes("end\\x") == "end\\x");           // nothing after \x
    CHECK(Utils::decodeHexEscapes("end\\x4") == "end\\x4");         // only 1 char after \x
    CHECK(Utils::decodeHexEscapes("bad\\xZZok") == "bad\\xZZok");   // not hex
    CHECK(Utils::decodeHexEscapes("bad\\xZZ\\x20ok") == "bad\\xZZ ok"); // invalid one kept, valid one after it decoded
}

TEST_CASE("decodeHexEscapes doesn't decode chars produced by decoding")
{
    // \x5c is '\' - the result "\x41" must stay, not be decoded again to "A"
    CHECK(Utils::decodeHexEscapes("\\x5cx41") == "\\x41");
}
