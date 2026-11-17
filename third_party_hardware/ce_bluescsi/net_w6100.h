#ifndef __NET_W6100_H__
#define __NET_W6100_H__

#include <Arduino.h>
#include <Client.h>
#include <Udp.h>
#include <IPAddress.h>

// The W6100 part of net.cpp (W6100-EVB-Pico / -Pico2) - only net.cpp calls these. In its own file: the Ethernet library's
// Ethernet.h and arduino-pico's WiFi.h can't be in one .cpp (both define EthernetLinkStatus, LinkON, LinkOFF...).

bool w6100Start(void);                      // finds the chip, no DHCP yet; false = no W6100
uint8_t w6100Process(uint8_t state);        // DHCP / lease renewal, returns the new NETST_* state
bool w6100LinkUp(void);
IPAddress w6100LocalIP(void);
IPAddress w6100SubnetMask(void);
Client& w6100Client(void);
UDP& w6100Udp(void);
void w6100BeforeConnect(void);

#endif
