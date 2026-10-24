#include <cstring>

#include "portal_form.h"

#define MAX_LEN         (MAX_SETTINGS_STRING_LEN - 1)
#define MIN_PASSWORD    8           // WPA2

static const char* pageStart =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
    "<title>Cosmo2ex FDD</title><style>"
    "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
    "input{width:100%;padding:.5em;margin:.3em 0 1em;box-sizing:border-box;font-size:1em}"
    "input[type=checkbox]{width:auto;margin:0 .5em 0 0}"
    "button{padding:.6em 2em;font-size:1em}.err{color:#c00}"
    "</style></head><body><h2>Cosmo2ex FDD - setup</h2>";

static const char* pageEnd = "</body></html>";

std::string htmlEscape(const char* text)
{
    std::string out;
    for(; text && *text; text++) {
        switch(*text) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += *text;    break;
        }
    }
    return out;
}

const char* portalApplyForm(const char* ssid, const char* password, bool ikbdEnabled, Settings_t& settings)
{
    size_t ssidLen = ssid ? strlen(ssid) : 0;
    size_t pswdLen = password ? strlen(password) : 0;

    if(ssidLen == 0 || ssidLen > MAX_LEN) {
        return "The WiFi name must have 1 to 31 characters.";
    }

    if(pswdLen > 0 && (pswdLen < MIN_PASSWORD || pswdLen > MAX_LEN)) {
        return "The password must have 8 to 31 characters, or be empty for an open network.";
    }

    bool sameSsid = strcmp(ssid, settings.ssid) == 0;

    if(pswdLen > 0) {
        strcpy(settings.password, password);
    } else if(!sameSsid) {
        settings.password[0] = 0;       // open network
    }                                   // else keep the stored password

    strcpy(settings.ssid, ssid);
    settings.ikbdEnabled = ikbdEnabled ? 1 : 0;
    return NULL;
}

std::string portalFormPage(const Settings_t& settings, const std::vector<std::string>& networks, const char* error)
{
    std::string page = pageStart;

    if(error) {
        page += "<p class='err'>" + htmlEscape(error) + "</p>";
    }

    page += "<form method='post' action='/save'>"
            "<label>WiFi name (SSID)<input name='ssid' list='nets' maxlength='31' autocomplete='off' value='";
    page += htmlEscape(settings.ssid);
    page += "'></label><datalist id='nets'>";
    for(const std::string& net : networks) {
        page += "<option value='" + htmlEscape(net.c_str()) + "'>";
    }
    page += "</datalist>"
            "<label>Password<input name='password' type='password' maxlength='31'></label>"
            "<p><small>Leave the password empty to keep the stored one (same WiFi name), or for an open network.</small></p>"
            "<p><label><input name='ikbd' type='checkbox' value='1'";
    page += settings.ikbdEnabled ? " checked" : "";
    page += ">Keyboard and mouse via the host (IKBD)</label></p>"
            "<button type='submit'>Save and connect</button></form>";

    page += pageEnd;
    return page;
}

std::string portalSavedPage(const char* ssid)
{
    std::string page = pageStart;
    page += "<p>Saved. The FDD now leaves this setup network and connects to <b>";
    page += htmlEscape(ssid);
    page += "</b> - see its display.</p>";
    page += pageEnd;
    return page;
}
