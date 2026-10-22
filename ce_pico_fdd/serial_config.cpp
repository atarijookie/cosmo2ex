#include <Arduino.h>      // Serial - the USB serial port
#include <cstring>

#include "hardware/watchdog.h"

#include "defs.h"
#include "utils.h"
#include "settings.h"
#include "display.h"
#include "serial_config.h"

void getString(char* buffer, int maxLen)
{
    memset(buffer, 0, maxLen);

    int i=0;
    while(1)
    {
        int key = Serial.read();

        if(key < 0) {       // nothing received yet
            continue;
        }

        if(key == '\n' || key == '\r' || i >= (maxLen-1)) {
            break;
        }

        Serial.write((char) key);   // echo back to console

        buffer[i] = key;
        i++;
    }
}

void serialShowMenu(void)
{
    Serial.printf("Serial config mode. Current stored settings are:\n");
    Serial.printf("--------------------------------------\n");
    Serial.printf("SSID    : %s\n", Settings.ssid);
    Serial.printf("password: %s\n", Settings.password);
    Serial.printf("ikbd    : %s\n", Settings.ikbdEnabled ? "enabled" : "disabled");
    Serial.printf("--------------------------------------\n");
    Serial.printf("Press 'S' to set SSID, 'P' to set password, 'I' to enable/disable IKBD, 'Q' to save.\n");
}

void serialConfigLoop(void)
{
    bool ssidChanged = false, pswdChanged = false, ikbdChanged = false;

    displayMessage("CONFIG MODE");

    loadSettingsFromEeprom();
    serialShowMenu();

    // main run loop
    while (1) {
        int key = Serial.read();

        if(key == '\n' || key == '\r') {
            serialShowMenu();
        }

        if(key == 's' || key == 'S') {
            Serial.printf("\nEnter SSID, finish by Enter key.\n");
            getString(Settings.ssid, MAX_SETTINGS_STRING_LEN);
            Serial.printf("\nNew SSID: %s\n", Settings.ssid);
            ssidChanged = true;
        }

        if(key == 'p' || key == 'P') {
            Serial.printf("\nEnter password, finish by Enter key.\n");
            getString(Settings.password, MAX_SETTINGS_STRING_LEN);
            Serial.printf("\nNew password: %s\n", Settings.password);
            pswdChanged = true;
        }

        if(key == 'i' || key == 'I') {
            Serial.printf("\nEnter 'E' to enable IKBD, 'D' to disable IKBD.\n");
            char ed[2];
            getString(ed, 2);

            if(ed[0] == 'e' || ed[0] == 'E') {
                Settings.ikbdEnabled = true;
                ikbdChanged = true;
            }

            if(ed[0] == 'd' || ed[0] == 'D') {
                Settings.ikbdEnabled = false;
                ikbdChanged = true;
            }

            Serial.printf("\nIKBD: %s\n", Settings.ikbdEnabled ? "enabled" : "disabled");
        }

        // on Q key
        if(key == 'q' || key == 'Q') {
            break;
        }
    }

    // if no settings change is detected, just quit
    if(!ssidChanged && !pswdChanged && !ikbdChanged) {
        Serial.printf("No settings changed.\n\n");
        return;
    }

    // some setting was changed, store it to flash - EEPROM.commit() pauses core 1 meanwhile, so it can be done right here
    saveSettingsToEeprom();

    debug("RESETing!\n");
    sleep_ms(20);
    watchdog_reboot(0, 0, 0);
}
