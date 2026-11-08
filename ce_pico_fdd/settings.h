#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <cstdint>

// Settings stored in flash (EEPROM library - the flash sector before the last one).

#define SETTINGS_VALID  0xCAFE
#define SETTINGS_LAYOUT 2       // the fields after ikbdEnabled are valid - older firmware didn't store them (a padding byte)

#define MAX_SETTINGS_STRING_LEN 32

typedef struct {
    uint16_t isValid;
    char ssid[MAX_SETTINGS_STRING_LEN];
    char password[MAX_SETTINGS_STRING_LEN];
    uint8_t ikbdEnabled;    // keyboard + mouse via the host
    uint8_t layout;         // SETTINGS_LAYOUT - otherwise the fields below get their defaults
    uint8_t beepOnStep;     // the beeper clicks on every head step
} Settings_t;

extern Settings_t Settings;

// Device settings as flags - sent to the host in the FW report, received from the host in ATN_DEVICE_SETTINGS (web UI).
#define DEV_SETTING_IKBD            0x01
#define DEV_SETTING_BEEP_ON_STEP    0x02

void loadSettingsFromEeprom(void);
void saveSettingsToEeprom(void);
void applySettings(void);                       // the current settings to the parts using them (beeper)

uint8_t deviceSettingsFlags(void);              // DEV_SETTING_* of the current settings
void deviceSettingsReceived(uint8_t flags);     // from the host: apply, store if changed

#endif
