#include <EEPROM.h>
#include <cstring>

#include "hardware/flash.h"

#include "defs.h"
#include "utils.h"
#include "settings.h"

#define EEPROM_SIZE     256     // must hold Settings_t

// The firmware before the arduino port kept the settings in the last flash sector, the EEPROM library uses the one before it.
#define OLD_SETTINGS_ADDRESS    (XIP_BASE + PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

Settings_t Settings;

// load settings from eeprom into struct - moving the old settings writes the flash, which is fine also while core 1 runs
// (EEPROM.commit() pauses it)
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

    // nothing stored via EEPROM yet - take the settings stored by the old firmware, if there are any, and store them here
    memcpy(&Settings, (const void*) OLD_SETTINGS_ADDRESS, sizeof(Settings));

    if(Settings.isValid == SETTINGS_VALID) {
        debug("loadSettingsFromEeprom - using settings from the old place\n");
        saveSettingsToEeprom();
        return;
    }

    memset(&Settings, 0, sizeof(Settings));
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
