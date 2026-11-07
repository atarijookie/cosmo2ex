#include <EEPROM.h>
#include <cstring>

#include "defs.h"
#include "utils.h"
#include "settings.h"

#define EEPROM_SIZE     256     // must hold Settings_t

Settings_t Settings;

// load settings from eeprom into struct
void loadSettingsFromEeprom(void)
{
    EEPROM.begin(EEPROM_SIZE);

    uint8_t* pSettings = (uint8_t*) &Settings;
    for(size_t i=0; i<sizeof(Settings); i++) {
        pSettings[i] = EEPROM.read(i);
    }

    if(Settings.isValid == SETTINGS_VALID) {
        return;
    }

    memset(&Settings, 0, sizeof(Settings));     // nothing stored - the defaults
    Settings.ikbdEnabled = 1;
}

// EEPROM.commit() stops core 1 while the flash is written (core 1 is started via setup1() / loop1(), so the arduino
// core can do that) and disables the interrupts on this core - the floppy streaming pauses for that time.
void saveSettingsToEeprom(void)
{
    Settings.isValid = SETTINGS_VALID;

    uint8_t* pSettings = (uint8_t*) &Settings;
    for(size_t i=0; i<sizeof(Settings); i++) {
        EEPROM.write(i, pSettings[i]);
    }

    if(!EEPROM.commit()) {
        debug("saveSettingsToEeprom - commit failed, settings not stored\n");
    }
}
