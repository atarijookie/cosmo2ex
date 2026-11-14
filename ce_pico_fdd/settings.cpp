#include <EEPROM.h>
#include <cstring>

#include "defs.h"
#include "utils.h"
#include "settings.h"
#include "beeper.h"
#include "neopixel.h"

#define EEPROM_SIZE     256     // must hold Settings_t

Settings_t Settings;

static uint8_t ledColorCheck(void)
{
    return LED_COLOR_CHECK ^ Settings.ledRunning[0] ^ Settings.ledRunning[1] ^ Settings.ledRunning[2];
}

static bool isBlack(const uint8_t* rgb)
{
    return (rgb[0] | rgb[1] | rgb[2]) == 0;
}

// garbage (not stored yet) or black (LED would look dead)? the default - in RAM only, stored when the host sends a color
static void ledColorValidate(void)
{
    if(Settings.ledCheck != ledColorCheck() || isBlack(Settings.ledRunning)) {
        Settings.ledRunning[0] = (uint8_t) (LED_RUNNING_DEFAULT >> 16);
        Settings.ledRunning[1] = (uint8_t) (LED_RUNNING_DEFAULT >> 8);
        Settings.ledRunning[2] = (uint8_t) LED_RUNNING_DEFAULT;
        Settings.ledCheck = ledColorCheck();
    }
}

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

    ledColorValidate();             // own check, not the layout - the byte after beepOnStep was padding before
    applySettings();
}

void applySettings(void)
{
    beeperSetStepClick(Settings.beepOnStep != 0);
    ledSetRunningColor(Settings.ledRunning);
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

// From the host on each connect, and when changed in the web UI. Stored only when different - the flash write pauses
// the floppy streaming.
void ledColorReceived(const uint8_t* rgb)
{
    if(isBlack(rgb)) {              // the host doesn't send black, so something's wrong - keep what we have
        return;
    }

    if(memcmp(Settings.ledRunning, rgb, 3) == 0) {
        return;
    }

    memcpy(Settings.ledRunning, rgb, 3);
    Settings.ledCheck = ledColorCheck();
    saveSettingsToEeprom();
    applySettings();

    debug("led color from host - %02X%02X%02X\n", rgb[0], rgb[1], rgb[2]);
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
