/*
    Tests of .env loading - line parsing (Utils::parseDotEnvLine) and ${VAR} substitution (Utils::dotEnvSubstituteVars)
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <map>
#include <string>

#include "doctest.h"

#include "misc/utils.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

// writes the content to a temp .env file, loads it into fresh dotEnv and substitutes vars - like Utils::loadDotEnv() does
static void loadDotEnvContent(const std::string& content)
{
    char path[] = "/tmp/ce_test_dotenv_XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);

    REQUIRE(write(fd, content.c_str(), content.length()) == (ssize_t) content.length());
    close(fd);

    dotEnv.clear();                                 // don't keep values from previous test
    bool good = Utils::loadDotEnvFrom(path);
    unlink(path);

    REQUIRE(good);
    Utils::dotEnvSubstituteVars();
}

// value of key from loaded .env; "<missing>" if not there, so missing and empty values can be told apart
static std::string val(const char* key)
{
    std::map<std::string, std::string>::iterator it = dotEnv.find(key);
    return (it != dotEnv.end()) ? it->second : std::string("<missing>");
}

//-------------------------------------------------------------------------------------------------
TEST_CASE("parseDotEnvLine splits key and value")
{
    std::string key, value;

    SUBCASE("simple") {
        CHECK(Utils::parseDotEnvLine("KEY=value\n", key, value));
        CHECK(key == "KEY");
        CHECK(value == "value");
    }

    SUBCASE("spaces inside value are kept") {
        CHECK(Utils::parseDotEnvLine("KEY=a b\n", key, value));
        CHECK(value == "a b");
    }

    SUBCASE("value can contain '='") {
        CHECK(Utils::parseDotEnvLine("KEY=a=b\n", key, value));
        CHECK(key == "KEY");
        CHECK(value == "a=b");
    }

    SUBCASE("blank chars around key and value are trimmed, also CR LF") {
        CHECK(Utils::parseDotEnvLine("  KEY = val  \r\n", key, value));
        CHECK(key == "KEY");
        CHECK(value == "val");
    }

    SUBCASE("empty value") {
        CHECK(Utils::parseDotEnvLine("KEY=\n", key, value));
        CHECK(key == "KEY");
        CHECK(value == "");
    }

    SUBCASE("vars are not replaced by parsing") {
        CHECK(Utils::parseDotEnvLine("KEY=${X:-/tmp/a}\n", key, value));
        CHECK(value == "${X:-/tmp/a}");
    }
}

TEST_CASE("parseDotEnvLine comments")
{
    std::string key, value;

    CHECK_FALSE(Utils::parseDotEnvLine("# comment\n", key, value));
    CHECK_FALSE(Utils::parseDotEnvLine("   # indented comment\n", key, value));

    SUBCASE("inline comment after blank char") {
        CHECK(Utils::parseDotEnvLine("KEY=v # comment\n", key, value));
        CHECK(value == "v");
    }

    SUBCASE("'#' without blank char before it is part of value") {
        CHECK(Utils::parseDotEnvLine("KEY=v#nocomment\n", key, value));
        CHECK(value == "v#nocomment");
    }
}

TEST_CASE("parseDotEnvLine quotes")
{
    std::string key, value;

    SUBCASE("double quotes are stripped, '#' and spaces inside are kept, comment after is ignored") {
        CHECK(Utils::parseDotEnvLine("KEY=\"x # y\" # c\n", key, value));
        CHECK(value == "x # y");
    }

    SUBCASE("single quotes are stripped") {
        CHECK(Utils::parseDotEnvLine("KEY='q=1'\n", key, value));
        CHECK(value == "q=1");
    }

    SUBCASE("unterminated quote is kept as it is") {
        CHECK(Utils::parseDotEnvLine("KEY=\"unterminated\n", key, value));
        CHECK(value == "\"unterminated");
    }
}

TEST_CASE("parseDotEnvLine skips lines without key-value pair")
{
    std::string key, value;

    CHECK_FALSE(Utils::parseDotEnvLine("\n", key, value));
    CHECK_FALSE(Utils::parseDotEnvLine("   \t \r\n", key, value));
    CHECK_FALSE(Utils::parseDotEnvLine("noeq\n", key, value));
    CHECK_FALSE(Utils::parseDotEnvLine("=v\n", key, value));
}

//-------------------------------------------------------------------------------------------------
TEST_CASE("substitution of ${VAR}")
{
    SUBCASE("simple var") {
        loadDotEnvContent("BASE=/opt/ce\nBIN=${BASE}/bin\n");
        CHECK(val("BIN") == "/opt/ce/bin");
    }

    SUBCASE("all vars in one value are replaced") {
        loadDotEnvContent("A=1\nB=2\nBOTH=${A}:${B}:${A}\n");
        CHECK(val("BOTH") == "1:2:1");
    }

    SUBCASE("'}' before var doesn't confuse finding var end") {
        loadDotEnvContent("BASE=/opt/ce\nBRACE=x}${BASE}\n");
        CHECK(val("BRACE") == "x}/opt/ce");
    }

    SUBCASE("text without vars is not changed") {
        loadDotEnvContent("PLAIN=no vars $HOME here\n");
        CHECK(val("PLAIN") == "no vars $HOME here");
    }

    SUBCASE("unclosed ${ is left as it is") {
        loadDotEnvContent("BASE=/opt/ce\nUNCLOSED=${BASE\n");
        CHECK(val("UNCLOSED") == "${BASE");
    }

    SUBCASE("unknown var without default is replaced with empty string") {
        unsetenv("CE_TEST_UNSET_VAR");
        loadDotEnvContent("X=a${CE_TEST_UNSET_VAR}b\n");
        CHECK(val("X") == "ab");
    }
}

TEST_CASE("substitution doesn't depend on order of keys")
{
    // keys are stored in map in alphabetical order - vars must be expanded recursively,
    // so a value referencing other value with vars works no matter which key comes first

    SUBCASE("referenced var with vars comes alphabetically later") {
        loadDotEnvContent("A_USES_Z=${Z_MID}/a\nZ_MID=${Z_TOP}/mid\nZ_TOP=/top\n");
        CHECK(val("A_USES_Z") == "/top/mid/a");
        CHECK(val("Z_MID") == "/top/mid");
    }

    SUBCASE("lines order in file doesn't matter") {
        loadDotEnvContent("LEAF=${MID}/leaf\nMID=${ROOT}/mid\nROOT=/root\n");
        CHECK(val("LEAF") == "/root/mid/leaf");
    }
}

TEST_CASE("substitution with default values")
{
    unsetenv("CE_TEST_UNSET_VAR");

    SUBCASE(":- uses default when var is not set") {
        loadDotEnvContent("X=${CE_TEST_UNSET_VAR:-def}\n");
        CHECK(val("X") == "def");
    }

    SUBCASE(":- uses default when var is empty") {
        loadDotEnvContent("EMPTY=\nX=${EMPTY:-def}\n");
        CHECK(val("X") == "def");
    }

    SUBCASE("- uses default only when var is not set") {
        loadDotEnvContent("EMPTY=\nX=${EMPTY-def}\nY=${CE_TEST_UNSET_VAR-def}\n");
        CHECK(val("X") == "");
        CHECK(val("Y") == "def");
    }

    SUBCASE("set var wins over default") {
        loadDotEnvContent("SET=value\nX=${SET:-def}\nY=${SET-def}\n");
        CHECK(val("X") == "value");
        CHECK(val("Y") == "value");
    }

    SUBCASE("default can contain '-'") {
        loadDotEnvContent("X=${CE_TEST_UNSET_VAR:-a-b}\nY=${CE_TEST_UNSET_VAR-c-d}\n");
        CHECK(val("X") == "a-b");
        CHECK(val("Y") == "c-d");
    }

    SUBCASE("default can contain other vars") {
        loadDotEnvContent("BASE=/opt/ce\nX=${CE_TEST_UNSET_VAR:-${BASE}/n}\n");
        CHECK(val("X") == "/opt/ce/n");
    }

    SUBCASE("empty default") {
        loadDotEnvContent("X=a${CE_TEST_UNSET_VAR:-}b\n");
        CHECK(val("X") == "ab");
    }
}

TEST_CASE("substitution with real env vars")
{
    SUBCASE("var not in .env is taken from env") {
        setenv("CE_TEST_ENV_VAR", "fromenv", 1);
        loadDotEnvContent("X=${CE_TEST_ENV_VAR}/x\nY=${CE_TEST_ENV_VAR:-def}\n");
        CHECK(val("X") == "fromenv/x");
        CHECK(val("Y") == "fromenv");
        unsetenv("CE_TEST_ENV_VAR");
    }

    SUBCASE("env var overrides default, like LOG_LEVEL=${CE_LOG_LEVEL:-3}") {
        setenv("CE_TEST_LOG_LEVEL", "4", 1);
        loadDotEnvContent("LOG_LEVEL=${CE_TEST_LOG_LEVEL:-3}\n");
        CHECK(val("LOG_LEVEL") == "4");

        unsetenv("CE_TEST_LOG_LEVEL");
        loadDotEnvContent("LOG_LEVEL=${CE_TEST_LOG_LEVEL:-3}\n");
        CHECK(val("LOG_LEVEL") == "3");
    }

    SUBCASE(".env value wins over env var with the same name") {
        setenv("CE_TEST_BOTH", "fromenv", 1);
        loadDotEnvContent("CE_TEST_BOTH=fromfile\nX=${CE_TEST_BOTH}\n");
        CHECK(val("X") == "fromfile");
        unsetenv("CE_TEST_BOTH");
    }
}

TEST_CASE("substitution of vars referencing each other in a loop terminates")
{
    loadDotEnvContent("LOOP1=${LOOP2}\nLOOP2=${LOOP1}\nSELF=${SELF}x\nOK=fine\n");

    // the values are not usable, but it must not hang or crash, and other values must be fine
    CHECK(val("OK") == "fine");
    CHECK(val("LOOP1") != "<missing>");
    CHECK(val("SELF") != "<missing>");
}

TEST_CASE("dotEnvValue returns substituted value, default or env var")
{
    unsetenv("CE_TEST_UNSET_VAR");
    loadDotEnvContent("BASE=/opt/ce\nBIN=${BASE}/bin\n");

    CHECK(Utils::dotEnvValue("BIN") == "/opt/ce/bin");
    CHECK(Utils::dotEnvValue("CE_TEST_UNSET_VAR", "default", false) == "default");
    CHECK(Utils::dotEnvValue("CE_TEST_UNSET_VAR", NULL, false) == "");

    setenv("CE_TEST_ENV_VAR", "fromenv", 1);
    CHECK(Utils::dotEnvValue("CE_TEST_ENV_VAR", "default", false) == "fromenv");
    unsetenv("CE_TEST_ENV_VAR");
}
