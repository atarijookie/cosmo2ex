#ifndef __NET_H__
#define __NET_H__

#include <Arduino.h>
#include <Client.h>
#include <Udp.h>
#include <IPAddress.h>

// The network for the host connection - the W6100 of the W6100-EVB-Pico / -Pico2: connection.cpp uses just the Arduino
// Client / UDP classes from here. Core 0 only.

#define NETST_STARTING      0
#define NETST_NO_HW         1       // W6100 not found (halted)
#define NETST_WAIT_IP       5       // link up, waiting for the IP from DHCP
#define NETST_READY         6       // got IP
#define NETST_NO_LINK       7       // ethernet cable not connected
#define NETST_NO_DHCP       8       // no answer from DHCP, retrying

bool netStart(void);                // in setup(): finds the W6100, no DHCP yet; false: no chip - can't work
void netProcess(void);              // from the main loop: DHCP, lease renewal, cable out / in
uint8_t netState(void);             // NETST_*
bool netReady(void);                // got IP
void netLogLinkInfo(void);          // link state to the log

IPAddress netLocalIP(void);
IPAddress netSubnetMask(void);
void netGetMac(uint8_t* mac);       // the MAC the host knows this device by

Client& netHddClient(void);         // the connection to the host
UDP& netUdp(void);                  // the host discovery
void netBeforeConnect(void);        // set up the HDD client for connect()
bool netHasRawNet(void);            // raw ethernet frames for STinG / MiNTnet possible (W6100 MACRAW) - sting_net.cpp

#endif
