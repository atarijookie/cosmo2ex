#ifndef __BOOT_STATUS_H__
#define __BOOT_STATUS_H__

#include <Arduino.h>

// Boot status: while not connected to the host, core 1 answers the ACSI / SCSI commands itself. The lowest enabled ID
// gets a root sector (sector 0) which shows the device status on the Atari and runs the real root sector once the host
// answers - see ce_pico_hdd/boot_status/status.s (boot_status_data.h is a copy). Sector 1 is the status itself.

#define BOOTST_STARTING         1       // setup, before the network init
#define BOOTST_NET_NO_HW        2       // network chip not found / not supported (halted)
#define BOOTST_NET_JOIN_FAILED  3       // can't join the WiFi network, retrying
#define BOOTST_WAIT_IP          4       // waiting for an IP address from DHCP
#define BOOTST_SEARCHING        6       // got IP, looking for the host
#define BOOTST_CONNECTING       7       // host found, connecting to it
#define BOOTST_NET_JOINING      8       // joining the WiFi network
#define BOOTST_NET_NO_CONFIG    9       // no WiFi network set (halted)

// core 0
void bootStatusSet(uint8_t code, const uint8_t* hostIp = NULL);     // hostIp: for BOOTST_CONNECTING
void bootStatusSetOwnIp(const uint8_t* ip);

// core 1, while not connected - true when the command was answered here, false: send it to core 0 as usual
bool bootStatusCommand(uint8_t* cmd, uint8_t cmdLen);

#endif
