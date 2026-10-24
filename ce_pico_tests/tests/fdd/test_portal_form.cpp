// portal_form.cpp - captive portal pages and the checks of the WiFi name / password / IKBD from the form: wrong values must
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
    CHECK(portalApplyForm("office", "newpassword", true, s) == nullptr);
    CHECK(std::string(s.ssid) == "office");
    CHECK(std::string(s.password) == "newpassword");
    CHECK(s.ikbdEnabled == 1);                  // the other settings stay
}

TEST_CASE("empty password - kept for the same network, open network for another one")
{
    Settings_t s = storedSettings("home", "oldpassword");
    CHECK(portalApplyForm("home", "", true, s) == nullptr);
    CHECK(std::string(s.password) == "oldpassword");

    CHECK(portalApplyForm("cafe", "", true, s) == nullptr);
    CHECK(std::string(s.ssid) == "cafe");
    CHECK(std::string(s.password) == "");
}

TEST_CASE("wrong values are rejected, the settings stay as they were")
{
    Settings_t s = storedSettings("home", "oldpassword");

    CHECK(portalApplyForm("", "newpassword", true, s) != nullptr);
    CHECK(portalApplyForm(nullptr, "newpassword", true, s) != nullptr);
    CHECK(portalApplyForm(std::string(32, 'x').c_str(), "newpassword", true, s) != nullptr);   // max 31
    CHECK(portalApplyForm("office", "short", true, s) != nullptr);                             // WPA2: min 8
    CHECK(portalApplyForm("office", std::string(32, 'p').c_str(), true, s) != nullptr);

    CHECK(std::string(s.ssid) == "home");
    CHECK(std::string(s.password) == "oldpassword");

    CHECK(portalApplyForm(std::string(31, 'x').c_str(), std::string(31, 'p').c_str(), true, s) == nullptr);
    CHECK(std::string(s.ssid) == std::string(31, 'x'));
}

TEST_CASE("ikbd checkbox - saved with valid values, unchanged with an error")
{
    Settings_t s = storedSettings("home", "oldpassword");     // ikbd enabled
    CHECK(portalApplyForm("home", "", false, s) == nullptr);
    CHECK(s.ikbdEnabled == 0);

    CHECK(portalApplyForm("", "", true, s) != nullptr);       // bad ssid - nothing changes
    CHECK(s.ikbdEnabled == 0);

    CHECK(portalApplyForm("home", "", true, s) == nullptr);
    CHECK(s.ikbdEnabled == 1);
}

TEST_CASE("form page - ikbd checkbox shows the stored setting")
{
    Settings_t s = storedSettings("home", "");
    CHECK(contains(portalFormPage(s, {}, nullptr), "name='ikbd' type='checkbox' value='1' checked>"));

    s.ikbdEnabled = 0;
    CHECK(contains(portalFormPage(s, {}, nullptr), "name='ikbd' type='checkbox' value='1'>"));
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
