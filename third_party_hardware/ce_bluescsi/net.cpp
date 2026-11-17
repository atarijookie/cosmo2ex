// Network for the host connection - see net.h. WiFi on the Pico W / Pico 2 W, W6100 on the W6100-EVB-Pico / -Pico2.

#include "defs.h"
#include "board.h"
#include "utils.h"
#include "net.h"

#if defined(PICO_CYW43_SUPPORTED)
    #define NET_HAS_WIFI    1
    #include <WiFi.h>
    #include <WiFiUdp.h>
#else
    #define NET_HAS_WIFI    0
#endif


// The WiFi credentials - for now from secrets.h, which is NOT in git (see .gitignore and secrets.example.h).
// Without it the device has no network (captive portal on SW1 will come later).
#if __has_include("secrets.h")
    #include "secrets.h"
#endif
#include "net_w6100.h"   // the W6100 part - in its own file, its Ethernet.h can't meet WiFi.h (both define EthernetLinkStatus)

#ifndef WIFI_SSID
    #define WIFI_SSID       ""
#endif
#ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD   ""
#endif

#define WIFI_JOIN_RETRY_MS      15000   // not joined after this long? try again
#define CONNECT_TIMEOUT_MS      3000    // connect() to host waits at most this long - it blocks the core 0 main loop
#define WRITE_TIMEOUT_MS        1000    // write() to host gives up after this long without progress
#define LINK_LOSS_GRACE_MS      5000    // the WiFi link gone for less than this - keep the host connection, the chip may get it back

static uint8_t state = NETST_STARTING;


static const char* ssid = WIFI_SSID;
static const char* password = WIFI_PASSWORD;

#if NET_HAS_WIFI
static WiFiClient wifiClientHdd;
static WiFiUDP wifiUdp;
static uint32_t lastJoinStart;
static uint32_t joinAttempts;
static uint32_t lastReadyMs;        // last time the link + IP were OK
static bool wasReady;               // had the link + IP at least once since the last join
static uint32_t linkLossCount;
#endif

// the network can't work - no chip / no config; these are never returned as usable, but the references must exist
class NullClient : public Client {
public:
    int connect(IPAddress, uint16_t) override { return 0; }
    int connect(const char*, uint16_t) override { return 0; }
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
    int available() override { return 0; }
    int read() override { return -1; }
    int read(uint8_t*, size_t) override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    void stop() override {}
    uint8_t connected() override { return 0; }
    operator bool() override { return false; }
};

class NullUdp : public UDP {
public:
    uint8_t begin(uint16_t) override { return 0; }
    void stop() override {}
    int beginPacket(IPAddress, uint16_t) override { return 0; }
    int beginPacket(const char*, uint16_t) override { return 0; }
    int endPacket() override { return 0; }
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
    int parsePacket() override { return 0; }
    int available() override { return 0; }
    int read() override { return -1; }
    int read(unsigned char*, size_t) override { return -1; }
    int read(char*, size_t) override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    IPAddress remoteIP() override { return IPAddress(0, 0, 0, 0); }
    uint16_t remotePort() override { return 0; }
};

static NullClient nullClient;
static NullUdp nullUdp;

//--------------------------------------------------------
#if NET_HAS_WIFI

static void wifiJoin(void)
{
    lastJoinStart = millis();
    joinAttempts++;
    wasReady = false;

    debug("net - joining WiFi '%s', attempt #%u\n", ssid, joinAttempts);

    // waits for joining the network (at most this long), not for the IP from DHCP - that comes later
    WiFi.setTimeout(5000);
    WiFi.beginNoBlock(ssid, password);
}

static void wifiProcess(void)
{
    int status = WiFi.status();
    IPAddress ip = WiFi.localIP();
    bool gotIp = (status == WL_CONNECTED) && (ip[0] | ip[1] | ip[2] | ip[3]) != 0;

    uint8_t newState;
    uint32_t now = millis();
    uint32_t sinceReady = now - lastReadyMs;    // before it's updated below - for the "link back" log

    if(gotIp) {
        newState = NETST_READY;
        lastReadyMs = now;
        lastJoinStart = now;    // a drop starts the rejoin timeout from now - not an instant rejoin, which kills the TCP connection
        wasReady = true;
    } else if(status == WL_CONNECTED) {
        newState = NETST_WAIT_IP;
    } else {
        newState = (joinAttempts > 1) ? NETST_JOIN_FAILED : NETST_JOINING;

        // failed, or still not joined after the retry time - try again
        bool failed = (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL);
        uint32_t retryMs = wasReady ? LINK_LOSS_GRACE_MS : WIFI_JOIN_RETRY_MS;  // link lost: give the chip 5 s to get it back
        if((failed && (millis() - lastJoinStart) > 2000) || (millis() - lastJoinStart) > retryMs) {
            if(failed) {
                newState = NETST_JOIN_FAILED;
            }
            wifiJoin();
        }
    }

    if(newState != state) {
        if(state == NETST_READY) {
            linkLossCount++;
            debug("net - LINK LOST #%u (WiFi status %d, IP %s), RSSI %d dBm\n", linkLossCount, status, ip.toString().c_str(),
                  (int) WiFi.RSSI());
        } else if(newState == NETST_READY && wasReady && linkLossCount > 0) {
            debug("net - link back after %u ms\n", sinceReady);
        }

        debug("net - state %d -> %d (WiFi status %d)\n", state, newState, status);

        if(newState == NETST_READY) {
            debug("net - IP: %s, mask: %s, RSSI: %d dBm\n", ip.toString().c_str(), WiFi.subnetMask().toString().c_str(),
                  (int) WiFi.RSSI());
            WiFi.noLowPowerMode();      // power save mode adds tens of ms to each packet from the host
        }

        state = newState;
    }
}

#endif


//--------------------------------------------------------

bool netStart(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        if(ssid[0] == 0) {
            debug("net - no WiFi SSID (secrets.h missing?)\n");
            state = NETST_NO_CONFIG;
            return false;
        }

        WiFi.mode(WIFI_STA);
        WiFi.setHostname("cosmo2ex-bluescsi");
        wifiJoin();
        state = NETST_JOINING;
        return true;
    }
#endif

    if(!BOARD_IS_WIFI) {
        state = w6100Start() ? NETST_WAIT_IP : NETST_NO_HW;
        return state != NETST_NO_HW;
    }

    debug("net - %s: built without WiFi support\n", board.name);   // Pico 2 W on the Pico 2 (W6100-EVB-Pico2) build
    state = NETST_NO_HW;
    return false;
}

void netProcess(void)
{
    if(state == NETST_NO_CONFIG || state == NETST_NO_HW) {
        return;
    }

#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        wifiProcess();
        return;
    }
#endif

    if(!BOARD_IS_WIFI) {
        uint8_t newState = w6100Process(state);
        if(newState != state) {
            debug("net - state %d -> %d\n", state, newState);
            state = newState;
        }
    }
}

uint8_t netState(void)
{
    return state;
}

bool netReady(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI && state != NETST_READY && wasReady && (millis() - lastReadyMs) < LINK_LOSS_GRACE_MS) {
        return true;    // a short drop of the WiFi link - let the TCP connection survive it
    }
#endif
    return state == NETST_READY;
}

// for the logs when the host connection is lost
void netLogLinkInfo(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        debug("net - WiFi status %d, state %d, RSSI %d dBm, link losses %u, ready %u ms ago\n", (int) WiFi.status(), state,
              (int) WiFi.RSSI(), linkLossCount, millis() - lastReadyMs);
        return;
    }
#endif
    if(!BOARD_IS_WIFI) {
        debug("net - W6100 link %s, state %d\n", w6100LinkUp() ? "up" : "down", state);
    }
}

const char* netName(void)
{
    return BOARD_IS_WIFI ? "WiFi" : "W6100";
}

const char* netSsid(void)
{
    return BOARD_IS_WIFI ? ssid : "";
}

IPAddress netLocalIP(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        return WiFi.localIP();
    }
#endif
    if(state == NETST_READY) {
        return w6100LocalIP();
    }
    return IPAddress(0, 0, 0, 0);
}

IPAddress netSubnetMask(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        return WiFi.subnetMask();
    }
#endif
    if(state == NETST_READY) {
        return w6100SubnetMask();
    }
    return IPAddress(0, 0, 0, 0);
}

void netGetMac(uint8_t* mac)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        WiFi.macAddress(mac);       // the WiFi chip's own MAC - only this one works on the WiFi network
        return;
    }
#endif
    memcpy(mac, settings.mac, 6);   // W6100 - our own MAC (from the board ID, see loadSettings())
}

Client& netHddClient(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        return wifiClientHdd;
    }
#endif
    if(!BOARD_IS_WIFI) {
        return w6100Client();
    }
    return nullClient;
}

void netBeforeConnect(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        wifiClientHdd.setTimeout(CONNECT_TIMEOUT_MS);   // for connect() - netAfterConnect() changes it for write()
        return;
    }
#endif
    w6100BeforeConnect();
}

UDP& netUdp(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        return wifiUdp;
    }
#endif
    if(!BOARD_IS_WIFI) {
        return w6100Udp();
    }
    return nullUdp;
}

void netAfterConnect(void)
{
#if NET_HAS_WIFI
    if(BOARD_IS_WIFI) {
        wifiClientHdd.setTimeout(WRITE_TIMEOUT_MS);
        wifiClientHdd.setNoDelay(true);         // small packets (commands, status) go out right away
        // not setSync(true) like FDD: it would wait for the ACK of each write - too slow for the sector data
    }
#endif
}

bool netHasRawNet(void)
{
    return !BOARD_IS_WIFI && state != NETST_NO_HW;
}
