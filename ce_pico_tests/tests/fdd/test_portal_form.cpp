// portal_form.cpp - captive portal pages and the checks of the WiFi name / password from the form: wrong values must
// not get into the settings, an empty password keeps the stored one only for the same network, and nothing entered
// (or found by the scan) may break the HTML.

#include <string>
#include <cstring>

#include "doctest.h"

#include "settings.h"
#include "portal_form.h"

namespace {

Settings_t storedSettings(const char* ssid, const char* password)
{
    Settings_t s;
    memset(&s, 0, sizeof(s));
    strcpy(s.ssid, ssid);
    strcpy(s.password, password);
    s.ikbdEnabled = 1;
    return s;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

}

TEST_CASE("new network with a password")
{
    Settings_t s = storedSettings("home", "oldpassword");
    CHECK(portalApplyCredentials("office", "newpassword", s) == nullptr);
    CHECK(std::string(s.ssid) == "office");
    CHECK(std::string(s.password) == "newpassword");
    CHECK(s.ikbdEnabled == 1);                  // the other settings stay
}

TEST_CASE("empty password - kept for the same network, open network for another one")
{
    Settings_t s = storedSettings("home", "oldpassword");
    CHECK(portalApplyCredentials("home", "", s) == nullptr);
    CHECK(std::string(s.password) == "oldpassword");

    CHECK(portalApplyCredentials("cafe", "", s) == nullptr);
    CHECK(std::string(s.ssid) == "cafe");
    CHECK(std::string(s.password) == "");
}

TEST_CASE("wrong values are rejected, the settings stay as they were")
{
    Settings_t s = storedSettings("home", "oldpassword");

    CHECK(portalApplyCredentials("", "newpassword", s) != nullptr);
    CHECK(portalApplyCredentials(nullptr, "newpassword", s) != nullptr);
    CHECK(portalApplyCredentials(std::string(32, 'x').c_str(), "newpassword", s) != nullptr);   // max 31
    CHECK(portalApplyCredentials("office", "short", s) != nullptr);                             // WPA2: min 8
    CHECK(portalApplyCredentials("office", std::string(32, 'p').c_str(), s) != nullptr);

    CHECK(std::string(s.ssid) == "home");
    CHECK(std::string(s.password) == "oldpassword");

    CHECK(portalApplyCredentials(std::string(31, 'x').c_str(), std::string(31, 'p').c_str(), s) == nullptr);
    CHECK(std::string(s.ssid) == std::string(31, 'x'));
}

TEST_CASE("html escaping")
{
    CHECK(htmlEscape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&#39;");
    CHECK(htmlEscape("") == "");
    CHECK(htmlEscape(nullptr) == "");
}

TEST_CASE("form page - stored ssid filled in and escaped, password never shown, scanned networks offered")
{
    Settings_t s = storedSettings("my'net<1>", "secretpassword");
    std::string page = portalFormPage(s, {"home", "a'b"}, nullptr);

    CHECK(contains(page, "value='my&#39;net&lt;1&gt;'"));
    CHECK_FALSE(contains(page, "secretpassword"));
    CHECK(contains(page, "<option value='home'>"));
    CHECK(contains(page, "<option value='a&#39;b'>"));
    CHECK(contains(page, "action='/save'"));
    CHECK_FALSE(contains(page, "class='err'"));
}

TEST_CASE("form page with an error, saved page")
{
    Settings_t s = storedSettings("home", "");
    std::string page = portalFormPage(s, {}, "bad <value>");
    CHECK(contains(page, "<p class='err'>bad &lt;value&gt;</p>"));

    CHECK(contains(portalSavedPage("o&o"), "<b>o&amp;o</b>"));
}
