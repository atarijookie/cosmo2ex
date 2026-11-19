// Network for the host connection - see net.h. The W6100 of the W6100-EVB-Pico / -Pico2, with the patched Ethernet
// library from third_party/Ethernet (the same as ce_pico_hdd).

#include <Ethernet.h>
#include <Dhcp.h>       // DHCP_CHECK_* results of Ethernet.maintain()
#include "w5100.h"      // W5100.init() - finds the chip without waiting for DHCP

#include "defs.h"
#include "utils.h"
#include "net.h"

#define W6100_PIN_CS            17      // W6100-EVB-Pico: SPI0 on GP16 (MISO), GP17 (CS), GP18 (SCK), GP19 (MOSI) - the
                                        // arduino-pico default SPI pins; RST GP20 and INT GP21 are not used
#define DHCP_TIMEOUT_MS         4000    // one DHCP attempt - blocks the core 0 main loop (core 1 answers the bus meanwhile)
#define DHCP_RESPONSE_MS        2000
#define DHCP_RETRY_MS           1000    // the next DHCP attempt this long after the previous one failed
#define CONNECT_TIMEOUT_MS      3000    // connect() to host waits at most this long - it blocks the core 0 main loop

static uint8_t state = NETST_STARTING;

static EthernetClient clientHdd;
static EthernetUDP udp;
static uint32_t lastTry;                // the last DHCP attempt, or the last maintain() when ready

static bool linkUp(void)
{
    return Ethernet.linkStatus() != LinkOFF;    // Unknown counts as up - DHCP finds out
}

static void setState(uint8_t newState)
{
    if(newState != state) {
        debug("net - state %d -> %d\n", state, newState);
        state = newState;
    }
}

bool netStart(void)
{
    Ethernet.init(W6100_PIN_CS);

    if(W5100.init() == 0 || Ethernet.hardwareStatus() == EthernetNoHardware) {
        debug("net - W6100 not found\n");
        state = NETST_NO_HW;
        return false;
    }

    debug("net - W6100 found (chip %d), link %s\n", (int) Ethernet.hardwareStatus(), linkUp() ? "up" : "down");

    state = NETST_WAIT_IP;
    lastTry = millis() - DHCP_RETRY_MS;     // DHCP right away in the first netProcess()
    return true;
}

void netProcess(void)
{
    if(state == NETST_NO_HW) {
        return;
    }

    uint32_t now = millis();

    if(state == NETST_READY) {
        if((now - lastTry) < 1000) {        // renew the DHCP lease when needed - check once per second
            return;
        }
        lastTry = now;

        int res = Ethernet.maintain();      // might block for a while when the DHCP server doesn't respond
        if(res == DHCP_CHECK_RENEW_FAIL || res == DHCP_CHECK_REBIND_FAIL) {
            debug("net - DHCP lease renewal failed (%d), will retry\n", res);
        }
        return;                             // a new IP after renew / rebind: connection.cpp sees it and reconnects
    }

    if((now - lastTry) < DHCP_RETRY_MS) {
        return;
    }

    if(!linkUp()) {                         // no cable - no point in asking DHCP
        lastTry = now;
        setState(NETST_NO_LINK);
        return;
    }

    debug("net - DHCP request\n");
    int gotIp = Ethernet.begin(settings.mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_MS);   // blocks at most DHCP_TIMEOUT_MS
    lastTry = millis();

    if(gotIp) {
        debug("net - IP: %s, mask: %s\n", Ethernet.localIP().toString().c_str(), Ethernet.subnetMask().toString().c_str());
        setState(NETST_READY);
    } else {
        setState(linkUp() ? NETST_NO_DHCP : NETST_NO_LINK);
    }
}

uint8_t netState(void)
{
    return state;
}

bool netReady(void)
{
    return state == NETST_READY;
}

void netLogLinkInfo(void)
{
    debug("net - W6100 link %s, state %d\n", linkUp() ? "up" : "down", state);
}

IPAddress netLocalIP(void)
{
    return (state == NETST_READY) ? Ethernet.localIP() : IPAddress(0, 0, 0, 0);
}

IPAddress netSubnetMask(void)
{
    return (state == NETST_READY) ? Ethernet.subnetMask() : IPAddress(0, 0, 0, 0);
}

void netGetMac(uint8_t* mac)
{
    memcpy(mac, settings.mac, 6);   // our own MAC (from the board ID, see loadSettings())
}

Client& netHddClient(void)
{
    return clientHdd;
}

UDP& netUdp(void)
{
    return udp;
}

void netBeforeConnect(void)
{
    clientHdd.setConnectionTimeout(CONNECT_TIMEOUT_MS);
}

bool netHasRawNet(void)
{
    return state != NETST_NO_HW;
}
