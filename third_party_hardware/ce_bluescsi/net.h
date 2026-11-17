#ifndef __NET_H__
#define __NET_H__

#include <Arduino.h>
#include <Client.h>
#include <Udp.h>
#include <IPAddress.h>

// The network for the host connection, the same for WiFi (Pico W / 2W) and W6100 (EVB-Pico, not implemented yet):
// connection.cpp uses just the Arduino Client / UDP classes from here. Core 0 only.

#define NETST_STARTING      0
#define NETST_NO_HW         1       // network chip not found / not supported yet
#define NETST_NO_CONFIG     2       // no WiFi SSID
#define NETST_JOINING       3       // joining the WiFi network (1st attempt)
#define NETST_JOIN_FAILED   4       // the last attempt to join failed, retrying
#define NETST_WAIT_IP       5       // joined / link up, waiting for the IP from DHCP
#define NETST_READY         6       // got IP

bool netStart(void);                // in setup(): starts joining the network, doesn't wait; false: can't work (no chip / no config)
void netProcess(void);              // from the main loop: rejoins the network when it's lost
uint8_t netState(void);             // NETST_*
bool netReady(void);                // got IP (WiFi: or had it less than 5 s ago - a short drop doesn't kill the host connection)
void netLogLinkInfo(void);          // WiFi status, RSSI... to the log
const char* netName(void);          // "WiFi" / "W6100" - for logs and the boot status
const char* netSsid(void);          // the WiFi network name, "" on W6100

IPAddress netLocalIP(void);
IPAddress netSubnetMask(void);
void netGetMac(uint8_t* mac);       // the MAC the host knows this device by

Client& netHddClient(void);         // the connection to the host
UDP& netUdp(void);                  // the host discovery
void netBeforeConnect(void);        // set up the HDD client for connect()
void netAfterConnect(void);         // set up the HDD client after a successful connect

#endif
