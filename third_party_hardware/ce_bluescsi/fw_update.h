#ifndef __FW_UPDATE_H__
#define __FW_UPDATE_H__

#include <Arduino.h>

// Firmware identity and update over the network (OTA) - the host (web UI) sends the new firmware over the existing
// HDD connection, the device stores it with arduino-pico's Updater into LittleFS, the OTA stub (first 12 kB of every
// arduino-pico image) copies it over the app on the next boot. Protocol: see the CMD_FW_UPDATE_* / ATN_FW_* in defs.h.
//
// Needs a firmware built with a file system ("flash size: 4MB (Sketch: 2MB, FS: 2MB)") - without it the device
// reports OTA as not possible, and the update must go via USB / debug probe.
//
// Rollback: before rebooting into the new firmware, the running one is kept in the file system. The new firmware is on
// trial until it was connected to the host for 10 s (a watchdog runs meanwhile, the trial ends with a reboot after 3 min).
// After 3 boots without that, the device goes back to the previous firmware and reports it to the host.

// device type - the same string is in the release file name: ce_pico_hdd_<FW_DEVICE_TYPE>.uf2 (the web UI makes the name
// from dev + type, so this firmware is an HDD too, just of another type)
#define FW_DEVICE_NAME      "HDD"
// 3 builds (see ce_bluescsi.ino):
// - RP2040, board Pico W:  runs on the Pico W (WiFi) and on the W6100-EVB-Pico (W6100) - detected at runtime
// - RP2350, board Pico 2W: Pico 2 W only (WiFi)
// - RP2350, board Pico 2:  W6100-EVB-Pico2 only (W6100)
#if defined(PICO_RP2350) && defined(PICO_CYW43_SUPPORTED)
    #define FW_DEVICE_TYPE  "BLUESCSI-PICO-2W"
    #define FW_CHIP         "RP2350"
#elif defined(PICO_RP2350)
    #define FW_DEVICE_TYPE  "BLUESCSI-W6100-EVB-PICO2"
    #define FW_CHIP         "RP2350"
#else
    #define FW_DEVICE_TYPE  "BLUESCSI-PICO-W"
    #define FW_CHIP         "RP2040"
#endif

#define FWU_CHUNK_MAX       1024        // max data bytes in one CMD_FW_UPDATE_DATA (+ 4 B offset) - the same as ce_pico_hdd

// results in ATN_FW_UPDATE_STATUS
#define FWU_OK              0
#define FWU_ERR_BUSY        1       // update can't start now
#define FWU_ERR_NO_FS       2       // firmware built without file system - no space for the new image
#define FWU_ERR_SIZE        3       // image too big / received size doesn't match
#define FWU_ERR_OFFSET      4       // chunk not at the expected offset
#define FWU_ERR_WRITE       5       // writing to the file system failed
#define FWU_ERR_IDENT       6       // image is not for this device (type), or has no identity
#define FWU_ERR_LAYOUT      7       // image has a different flash layout (file system / EEPROM position)
#define FWU_ERR_MD5         8       // MD5 of the received image doesn't match
#define FWU_ERR_BEGIN       9       // Updater.begin() failed
#define FWU_ERR_TIMEOUT     10      // chunk data didn't arrive in time
#define FWU_ERR_NOT_RUNNING 11      // data / end without start

void fwInfoInit(void);                              // in setup(): size + MD5 of the running image, trial / rollback state
void fwTrialLoop(bool hostConnected);               // from the main loop - confirms a new firmware, or reboots it
void fwInfoResetSent(void);                         // new connection to host - send the info again
void fwInfoSendIfNeeded(void);                      // once per connection, after the FW version (host knows the MAC then)

bool fwUpdateIsCommand(uint16_t cmdCode);
uint32_t fwUpdatePayloadToWaitFor(uint16_t cmdCode, uint32_t len);     // see payloadSizeToWaitFor() in connection.cpp
void fwUpdateCommand(uint16_t cmdCode, uint32_t len);                  // handle CMD_FW_UPDATE_*, reads its payload
void fwUpdateAbort(const char* why);                                    // e.g. connection lost

#endif
