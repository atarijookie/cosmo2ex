#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <algorithm>

#include "defs.h"
#include "utils.h"
#include "settings.h"
#include "display.h"
#include "panel.h"
#include "connection.h"
#include "portal_form.h"
#include "captive_portal.h"

#define MAX_NETWORKS    16

static const IPAddress apIp(192, 168, 42, 1);

static DNSServer dnsServer;
static WebServer webServer(80);

static bool active;
static uint32_t lastRequest;
static bool saved;
static uint32_t savedTime;
static std::vector<std::string> networks;     // from the scan before the AP starts, strongest first

static void showOnPanel(void)
{
    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, "WIFI SETUP");
    panelSetLine(1, PANEL_LEFT, 0, "");
    panelSetLine(2, PANEL_LEFT, 0, "AP: " PORTAL_AP_NAME);
    panelSetLine(3, PANEL_LEFT, 0, "http://192.168.42.1");
}

// the AP can't scan, so the networks for the form are scanned before it starts
static void scanNetworks(void)
{
    networks.clear();

    int count = WiFi.scanNetworks();
    if(count <= 0) {
        debug("captive portal - no networks found (%d)\n", count);
        return;
    }

    std::vector<std::pair<int32_t, std::string>> found;
    for(int i = 0; i < count; i++) {
        std::string ssid = WiFi.SSID(i);
        if(ssid.empty()) {          // hidden network
            continue;
        }

        auto same = std::find_if(found.begin(), found.end(), [&](const auto& f) { return f.second == ssid; });
        if(same == found.end()) {
            found.push_back({WiFi.RSSI(i), ssid});
        } else if(WiFi.RSSI(i) > same->first) {
            same->first = WiFi.RSSI(i);
        }
    }
    WiFi.scanDelete();

    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for(size_t i = 0; i < found.size() && i < MAX_NETWORKS; i++) {
        networks.push_back(found[i].second);
    }
    debug("captive portal - %d networks found\n", (int) networks.size());
}

static void sendPage(const std::string& page)
{
    webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    webServer.send(200, "text/html", page.c_str());
}

static void handleRoot(void)
{
    lastRequest = millis();
    sendPage(portalFormPage(Settings, networks, NULL));
}

static void handleSave(void)
{
    lastRequest = millis();

    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");

    Settings_t newSettings = Settings;
    const char* error = portalApplyCredentials(ssid.c_str(), password.c_str(), newSettings);

    if(error) {
        sendPage(portalFormPage(Settings, networks, error));
        return;
    }

    Settings = newSettings;
    saveSettingsToEeprom();
    debug("captive portal - saved, ssid: %s\n", Settings.ssid);

    sendPage(portalSavedPage(Settings.ssid));

    displayMessage("wifi setup saved", "connecting to:", Settings.ssid);
    saved = true;
    savedTime = millis();
}

// any other URL (also the phones' checks for internet, e.g. /generate_204, /hotspot-detect.html) - to the form,
// which makes the phone show the portal
static void handleOther(void)
{
    lastRequest = millis();
    webServer.sendHeader("Location", "http://192.168.42.1/", true);
    webServer.send(302, "text/plain", "redirect");
}

void startCaptivePortal(void)
{
    if(active) {
        return;
    }

    debug("captive portal - starting\n");
    displayMessage("WIFI SETUP", "scanning networks...");

    connectionStop();           // no host while in AP mode
    scanNetworks();

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    if(!WiFi.softAP(PORTAL_AP_NAME)) {
        debug("captive portal - AP start failed\n");
        displayMessage("WIFI SETUP", "AP start failed");
        WiFi.mode(WIFI_STA);
        connectionRestart();
        return;
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIp);

    static bool handlersSet = false;
    if(!handlersSet) {
        handlersSet = true;
        webServer.on("/", HTTP_GET, handleRoot);
        webServer.on("/save", HTTP_POST, handleSave);
        webServer.onNotFound(handleOther);
    }
    webServer.begin();

    active = true;
    saved = false;
    lastRequest = millis();
    showOnPanel();
    debug("captive portal - AP %s on %s\n", PORTAL_AP_NAME, apIp.toString().c_str());
}

void stopCaptivePortal(void)
{
    if(!active) {
        return;
    }

    debug("captive portal - stopping\n");

    webServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect();
    WiFi.end();
    WiFi.mode(WIFI_STA);

    active = false;

    if(!saved) {
        displayMessage("wifi setup", "not changed");
    }
    connectionRestart();        // connect with the stored settings right away
}

void toggleCaptivePortal(void)
{
    if(active) {
        stopCaptivePortal();
    } else {
        startCaptivePortal();
    }
}

bool captivePortalActive(void)
{
    return active;
}

void captivePortalLoop(void)
{
    dnsServer.processNextRequest();
    webServer.handleClient();

    uint32_t now = millis();

    if(saved && (now - savedTime) >= PORTAL_SAVED_MS) {
        stopCaptivePortal();
        return;
    }

    if(!saved && (now - lastRequest) >= PORTAL_TIMEOUT_MS) {
        debug("captive portal - timeout\n");
        stopCaptivePortal();
    }
}
