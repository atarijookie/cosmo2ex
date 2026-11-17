// W6100 on the W6100-EVB-Pico / -Pico2 - the network part of net.cpp for these boards, see net_w6100.h.
// The patched Ethernet library from third_party/Ethernet (the same as ce_pico_hdd). It's in every build - the RP2040 build
// runs on both the Pico W and the W6100-EVB-Pico (detected at runtime, see board.cpp).

#include <Ethernet.h>
#include <Dhcp.h>       // DHCP_CHECK_* results of Ethernet.maintain()
#include "w5100.h"      // W5100.init() - finds the chip without waiting for DHCP

#include "defs.h"
#include "utils.h"
#include "net.h"
#include "net_w6100.h"

#define W6100_PIN_CS            17      // W6100-EVB-Pico: SPI0 on GP16 (MISO), GP17 (CS), GP18 (SCK), GP19 (MOSI) - the
                                        // arduino-pico default SPI pins; RST GP20 and INT GP21 are not used
#define DHCP_TIMEOUT_MS         4000    // one DHCP attempt - blocks the core 0 main loop (core 1 answers the bus meanwhile)
#define DHCP_RESPONSE_MS        2000
#define DHCP_RETRY_MS           1000    // the next DHCP attempt this long after the previous one failed
#define CONNECT_TIMEOUT_MS      3000

static EthernetClient clientHdd;
static EthernetUDP udp;
static uint32_t lastTry;                // the last DHCP attempt, or the last maintain() when ready

bool w6100Start(void)
{
    Ethernet.init(W6100_PIN_CS);

    if(W5100.init() == 0 || Ethernet.hardwareStatus() == EthernetNoHardware) {
        debug("net - W6100 not found\n");
        return false;
    }

    debug("net - W6100 found (chip %d), link %s\n", (int) Ethernet.hardwareStatus(), w6100LinkUp() ? "up" : "down");

    lastTry = millis() - DHCP_RETRY_MS;     // DHCP right away in the first w6100Process()
    return true;
}

uint8_t w6100Process(uint8_t state)
{
    uint32_t now = millis();

    if(state == NETST_READY) {
        if((now - lastTry) < 1000) {        // renew the DHCP lease when needed - check once per second
            return state;
        }
        lastTry = now;

        int res = Ethernet.maintain();      // might block for a while when the DHCP server doesn't respond
        if(res == DHCP_CHECK_RENEW_FAIL || res == DHCP_CHECK_REBIND_FAIL) {
            debug("net - DHCP lease renewal failed (%d), will retry\n", res);
        }
        return state;                       // a new IP after renew / rebind: connection.cpp sees it and reconnects
    }

    if((now - lastTry) < DHCP_RETRY_MS) {
        return state;
    }

    if(!w6100LinkUp()) {                    // no cable - no point in asking DHCP
        lastTry = now;
        return NETST_NO_LINK;
    }

    debug("net - DHCP request\n");
    int gotIp = Ethernet.begin(settings.mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_MS);   // blocks at most DHCP_TIMEOUT_MS
    lastTry = millis();

    if(gotIp) {
        debug("net - IP: %s, mask: %s\n", Ethernet.localIP().toString().c_str(), Ethernet.subnetMask().toString().c_str());
        return NETST_READY;
    }

    return w6100LinkUp() ? NETST_NO_DHCP : NETST_NO_LINK;
}

bool w6100LinkUp(void)
{
    return Ethernet.linkStatus() != LinkOFF;    // Unknown counts as up - DHCP finds out
}

IPAddress w6100LocalIP(void)
{
    return Ethernet.localIP();
}

IPAddress w6100SubnetMask(void)
{
    return Ethernet.subnetMask();
}

Client& w6100Client(void)
{
    return clientHdd;
}

UDP& w6100Udp(void)
{
    return udp;
}

void w6100BeforeConnect(void)
{
    clientHdd.setConnectionTimeout(CONNECT_TIMEOUT_MS);
}
