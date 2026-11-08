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
#include "neopixel.h"

#define MAX_NETWORKS    16

static const IPAddress apIp(192, 168, 42, 1);

static DNSServer dnsServer;
static WebServer webServer(80);

static bool active;                     // from the start of the network scan
static bool scanning;
static uint32_t scanStart;
static uint32_t lastRequest;
static bool saved;
static bool leaving;                    // saved or quit - the portal stops PORTAL_LEAVE_MS after leaveTime
static uint32_t leaveTime;
static std::vector<std::string> networks;     // from the scan before the AP starts, strongest first

static bool noWifiSettings(void)
{
    return Settings.ssid[0] == 0;
}

static void showOnPanel(void)
{
    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, "WIFI SETUP");
    panelSetLine(1, PANEL_LEFT, 0, "");
    panelSetLine(2, PANEL_LEFT, 0, "AP: " PORTAL_AP_NAME);
    panelSetLine(3, PANEL_LEFT, 0, "http://192.168.42.1");
}

// the AP can't scan, so the networks for the form are scanned before it starts - in the background, the main loop
// keeps running (the button, the LED, the keyboard)
static void collectScannedNetworks(void)
{
    networks.clear();

    int count = WiFi.scanComplete();
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
    bool ikbd = webServer.hasArg("ikbd");      // an unchecked checkbox isn't sent at all
    bool beep = webServer.hasArg("beep");

    Settings_t newSettings = Settings;
    const char* error = portalApplyForm(ssid.c_str(), password.c_str(), ikbd, beep, newSettings);

    if(error) {
        sendPage(portalFormPage(Settings, networks, error));
        return;
    }

    Settings = newSettings;
    saveSettingsToEeprom();
    applySettings();
    debug("captive portal - saved, ssid: %s, ikbd: %d, beep on step: %d\n", Settings.ssid, Settings.ikbdEnabled, Settings.beepOnStep);

    sendPage(portalSavedPage(Settings.ssid));

    displayMessage("wifi setup saved", "connecting to:", Settings.ssid);
    saved = true;
    leaving = true;
    leaveTime = millis();
}

// leave without saving - only with WiFi settings stored (the button is shown only then), otherwise back to the form
static void handleQuit(void)
{
    lastRequest = millis();

    if(noWifiSettings()) {
        sendPage(portalFormPage(Settings, networks, "No WiFi settings saved yet - please enter them."));
        return;
    }

    debug("captive portal - quit without saving\n");
    sendPage(portalQuitPage(Settings.ssid));

    leaving = true;
    leaveTime = millis();
}

// any other URL (also the phones' checks for internet, e.g. /generate_204, /hotspot-detect.html) - to the form,
// which makes the phone show the portal
static void handleOther(void)
{
    lastRequest = millis();
    webServer.sendHeader("Location", "http://192.168.42.1/", true);
    webServer.send(302, "text/plain", "redirect");
}

// softAPConfig() switches the interface to a static IP, and that stays after WiFi.end() - back to DHCP, otherwise the
// station joins the WiFi with the AP address (192.168.42.1) and never asks for its own. Needs the interface stopped.
static void wifiBackToStation(void)
{
    WiFi.end();
    WiFi.config(IPAddress());
    WiFi.mode(WIFI_STA);
}

void startCaptivePortal(void)
{
    if(active) {
        return;
    }

    debug("captive portal - starting\n");
    displayMessage("WIFI SETUP", "scanning networks...");
    ledSetCaptivePortal();

    connectionStop();           // no host while in AP mode

    WiFi.scanNetworks(true);
    scanning = true;
    scanStart = millis();
    active = true;
}

static void startAccessPoint(void)
{
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    if(!WiFi.softAP(PORTAL_AP_NAME)) {
        debug("captive portal - AP start failed\n");
        displayMessage("WIFI SETUP", "AP start failed");
        wifiBackToStation();
        active = false;
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
        webServer.on("/quit", HTTP_POST, handleQuit);
        webServer.onNotFound(handleOther);
    }
    webServer.begin();

    saved = false;
    leaving = false;
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
    wifiBackToStation();

    active = false;

    if(!saved) {
        displayMessage("wifi setup", "not changed");
    }
    connectionRestart();        // connect with the stored settings right away
}

bool captivePortalCanToggle(void)
{
    return !scanning && !(active && noWifiSettings());
}

void toggleCaptivePortal(void)
{
    if(!captivePortalCanToggle()) {
        debug("captive portal - can't leave it now\n");
        return;
    }

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
    if(scanning) {
        if(WiFi.scanComplete() < 0 && (millis() - scanStart) < PORTAL_SCAN_MS) {
            return;
        }

        scanning = false;
        collectScannedNetworks();
        startAccessPoint();
        return;
    }

    dnsServer.processNextRequest();
    webServer.handleClient();

    uint32_t now = millis();

    if(leaving && (now - leaveTime) >= PORTAL_LEAVE_MS) {
        stopCaptivePortal();
        return;
    }

    if(!leaving && !noWifiSettings() && (now - lastRequest) >= PORTAL_TIMEOUT_MS) {
        debug("captive portal - timeout\n");
        stopCaptivePortal();
    }
}

void captivePortalAutoStart(void)
{
    static bool tried = false;
    static uint32_t lastStart = 0;

    if(active || !noWifiSettings()) {
        return;
    }

    uint32_t now = millis();
    if(tried && (now - lastStart) < PORTAL_RETRY_MS) {     // the AP failed to start last time - not on every pass
        return;
    }

    tried = true;
    lastStart = now;
    debug("captive portal - no WiFi settings stored, starting it\n");
    startCaptivePortal();
}
