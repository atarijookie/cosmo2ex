/*
    Tests of small helpers from misc/utils.cpp
*/
#include <string>
#include <map>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "doctest.h"

#include "misc/utils.h"
#include "misc/settings.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

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

static uint16_t atariDate(int year, int month, int day)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    return Utils::fileTimeToAtariDate(&t);
}

static uint16_t atariDateOf(int year, int month, int day)     // expected value: GEMDOS date bits
{
    return ((year - 1980) << 9) | (month << 5) | day;
}

TEST_CASE("fileTimeToAtariDate - dates within GEMDOS range are converted")
{
    CHECK(atariDate(1980, 1, 1)   == atariDateOf(1980, 1, 1));
    CHECK(atariDate(2024, 6, 15)  == atariDateOf(2024, 6, 15));
    CHECK(atariDate(2107, 12, 31) == atariDateOf(2107, 12, 31));
}

TEST_CASE("fileTimeToAtariDate - dates outside GEMDOS range are clamped")
{
    CHECK(atariDate(1970, 1, 1)  == atariDateOf(1980, 1, 1));       // used to become year 2098
    CHECK(atariDate(1979, 12, 31) == atariDateOf(1980, 1, 1));
    CHECK(atariDate(2108, 1, 1)  == atariDateOf(2107, 12, 31));     // used to wrap to 1980
    CHECK(atariDate(2200, 5, 5)  == atariDateOf(2107, 12, 31));
}

TEST_CASE("setTimezoneVariable_inThisContext - UTC offset from settings applies to localtime right away")
{
    char tmpl[] = "/tmp/ce_test_tz_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != NULL);
    dotEnv["SETTINGS_DIR"] = tmpl;

    const char *oldTz = getenv("TZ");
    std::string oldTzCopy = oldTz ? oldTz : "";

    Settings s;
    s.setFloat("TIME_UTC_OFFSET", 2.0f);
    Utils::setTimezoneVariable_inThisContext();

    time_t t = 0;                       // 1970-01-01 00:00 UTC
    struct tm lt;
    localtime_r(&t, &lt);
    CHECK(lt.tm_hour == 2);             // UTC+2

    if(oldTz) {                         // restore
        setenv("TZ", oldTzCopy.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    dotEnv.erase("SETTINGS_DIR");
    std::string cmd = std::string("rm -rf '") + tmpl + "'";
    system(cmd.c_str());
}
