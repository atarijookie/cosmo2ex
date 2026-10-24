#ifndef __PORTAL_FORM_H__
#define __PORTAL_FORM_H__

#include <string>
#include <vector>

#include "settings.h"

// The pages of the captive portal and the checks of the entered values - no WiFi / web server here, tested on the host.

std::string htmlEscape(const char* text);

// Checks the values from the form and puts them into settings. An empty password keeps the stored one when the SSID
// didn't change, otherwise it means an open network. Returns NULL when ok, or the error to show - settings unchanged then.
const char* portalApplyForm(const char* ssid, const char* password, bool ikbdEnabled, Settings_t& settings);

std::string portalFormPage(const Settings_t& settings, const std::vector<std::string>& networks, const char* error);
std::string portalSavedPage(const char* ssid);

#endif
