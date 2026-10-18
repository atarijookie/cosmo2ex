/*
    Tests of the command socket token check from misc/cmdsockthread.cpp
*/
#include <string>

#include "doctest.h"

#include "misc/cmdsockthread.h"

TEST_CASE("cmdSockStripToken - valid token returns the json after it")
{
    const char* msg = "0123abcd\n{\"module\":\"all\"}";
    const char* json = cmdSockStripToken(msg, "0123abcd");
    REQUIRE(json != NULL);
    CHECK(std::string(json) == "{\"module\":\"all\"}");
}

TEST_CASE("cmdSockStripToken - rejects wrong, partial, missing or empty token")
{
    CHECK(cmdSockStripToken("0123abce\n{}", "0123abcd") == NULL);   // one char differs
    CHECK(cmdSockStripToken("0123abc\n{}",  "0123abcd") == NULL);   // too short
    CHECK(cmdSockStripToken("0123abcdX\n{}", "0123abcd") == NULL);  // too long
    CHECK(cmdSockStripToken("{\"module\":\"all\"}", "0123abcd") == NULL);  // no token line
    CHECK(cmdSockStripToken("\n{}", "") == NULL);                   // token not created - reject everything
}
