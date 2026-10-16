// Host replacement for the Ethernet library (third_party/Ethernet is NOT compiled into the tests).
// Mimics only the behaviour the ce_pico_hdd_* code relies on:
//  - EthernetClient: rx bytes are queued by the test (rx), whatever the code writes ends in tx.
//    read(buf, n) returns -1 when connected and nothing is available, same as the W5x00 socketRecv().
//  - EthernetUDP: sent packets are recorded, received packets are queued by the test.

#ifndef ETHERNET_FAKE_H
#define ETHERNET_FAKE_H

#include <deque>
#include <vector>
#include <string>
#include "Arduino.h"
#include "Dhcp.h"

class EthernetClient
{
public:
    std::deque<uint8_t>  rx;                // data from host, waiting to be read by the device
    std::vector<uint8_t> tx;                // data the device sent to host
    bool                 isConnected   = false;
    bool                 connectResult = true;
    std::string          connectHost;
    uint16_t             connectPort   = 0;
    int                  connectCount  = 0;
    int                  stopCount     = 0;

    void reset(void) { *this = EthernetClient(); }

    void pushRx(const std::vector<uint8_t> &data) { rx.insert(rx.end(), data.begin(), data.end()); }

    int connect(const char *host, uint16_t port)
    {
        connectHost = host;
        connectPort = port;
        connectCount++;
        isConnected = connectResult;
        return isConnected;
    }

    uint8_t connected(void) { return isConnected; }
    void stop(void) { stopCount++; isConnected = false; }

    int available(void) { return rx.size(); }

    int read(void)
    {
        if (rx.empty()) return -1;
        uint8_t b = rx.front();
        rx.pop_front();
        return b;
    }

    int read(uint8_t *buf, size_t size)
    {
        if (rx.empty()) return isConnected ? -1 : 0;

        size_t n = MIN(size, rx.size());
        for (size_t i = 0; i < n; i++) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    size_t write(const uint8_t *buf, size_t size)
    {
        if (!isConnected) return 0;
        tx.insert(tx.end(), buf, buf + size);
        return size;
    }
};

class EthernetUDP
{
public:
    struct Packet {
        std::string          host;
        uint16_t             port;
        std::vector<uint8_t> data;
    };

    bool                 begun     = false;
    uint16_t             localPort = 0;
    std::vector<Packet>  sent;
    std::deque<Packet>   incoming;          // host field is the remote ip in dotted form

    void reset(void) { *this = EthernetUDP(); }

    uint8_t begin(uint16_t port) { begun = true; localPort = port; return 1; }
    void stop(void) { begun = false; }

    int beginPacket(const char *host, uint16_t port) { current = Packet{host, port, {}}; return 1; }
    size_t write(const uint8_t *buf, size_t size) { current.data.insert(current.data.end(), buf, buf + size); return size; }
    int endPacket(void) { sent.push_back(current); return 1; }

    int parsePacket(void)
    {
        if (incoming.empty()) return 0;
        rxPacket = incoming.front();
        incoming.pop_front();
        rxPos = 0;
        return rxPacket.data.size();
    }

    int read(uint8_t *buf, size_t len)
    {
        size_t n = MIN(len, rxPacket.data.size() - rxPos);
        memcpy(buf, rxPacket.data.data() + rxPos, n);
        rxPos += n;
        return n;
    }

    IPAddress remoteIP(void)
    {
        unsigned a = 0, b = 0, c = 0, d = 0;
        sscanf(rxPacket.host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
        return IPAddress(a, b, c, d);
    }

private:
    Packet current;
    Packet rxPacket;
    size_t rxPos = 0;
};

enum EthernetHardwareStatus { EthernetNoHardware, EthernetW5100, EthernetW5200, EthernetW5500 };
enum EthernetLinkStatus { Unknown, LinkON, LinkOFF };

class EthernetClass
{
public:
    IPAddress ip   = IPAddress(192, 168, 1, 50);
    IPAddress mask = IPAddress(255, 255, 255, 0);

    int       maintainCount  = 0;
    int       maintainResult = DHCP_CHECK_NONE;     // what the next maintain() returns
    IPAddress maintainNewIp;                        // ip after a successful renew / rebind, 0.0.0.0 = keep the current one

    IPAddress localIP(void) { return ip; }
    IPAddress subnetMask(void) { return mask; }

    int maintain(void)
    {
        maintainCount++;

        int res = maintainResult;
        if ((res == DHCP_CHECK_RENEW_OK || res == DHCP_CHECK_REBIND_OK) && maintainNewIp != IPAddress()) {
            ip = maintainNewIp;
        }

        maintainResult = DHCP_CHECK_NONE;
        maintainNewIp  = IPAddress();
        return res;
    }
};

extern EthernetClass Ethernet;

#endif
