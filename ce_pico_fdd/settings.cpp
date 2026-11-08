#include <EEPROM.h>
#include <cstring>

#include "defs.h"
#include "utils.h"
#include "settings.h"
#include "beeper.h"

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

    if(Settings.isValid != SETTINGS_VALID) {
        memset(&Settings, 0, sizeof(Settings));     // nothing stored - the defaults
        Settings.ikbdEnabled = 1;
    }

    if(Settings.layout != SETTINGS_LAYOUT) {        // stored by older firmware, or the defaults - the newer fields too
        Settings.layout = SETTINGS_LAYOUT;
        Settings.beepOnStep = 1;
    }

    applySettings();
}

void applySettings(void)
{
    beeperSetStepClick(Settings.beepOnStep != 0);
}

uint8_t deviceSettingsFlags(void)
{
    return (Settings.ikbdEnabled ? DEV_SETTING_IKBD : 0) | (Settings.beepOnStep ? DEV_SETTING_BEEP_ON_STEP : 0);
}

// From the host (web UI). Stored only when something changed - the flash write pauses the floppy streaming.
void deviceSettingsReceived(uint8_t flags)
{
    uint8_t ikbd = (flags & DEV_SETTING_IKBD) ? 1 : 0;
    uint8_t beep = (flags & DEV_SETTING_BEEP_ON_STEP) ? 1 : 0;

    if(Settings.ikbdEnabled == ikbd && Settings.beepOnStep == beep) {
        return;
    }

    Settings.ikbdEnabled = ikbd;
    Settings.beepOnStep = beep;
    saveSettingsToEeprom();
    applySettings();

    debug("settings from host - ikbd: %d, beep on step: %d\n", ikbd, beep);
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
