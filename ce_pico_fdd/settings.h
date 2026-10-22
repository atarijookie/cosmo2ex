#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <cstdint>

// Settings stored in flash (EEPROM library - the last flash sector, same place and layout as before the arduino port).

#define SETTINGS_VALID  0xCAFE

#define MAX_SETTINGS_STRING_LEN 32

typedef struct {
    uint16_t isValid;
    char ssid[MAX_SETTINGS_STRING_LEN];
    char password[MAX_SETTINGS_STRING_LEN];
    uint8_t ikbdEnabled;
} Settings_t;

extern Settings_t Settings;

void loadSettingsFromEeprom(void);
void saveSettingsToEeprom(void);

#endif
