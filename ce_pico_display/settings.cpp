#include <EEPROM.h>

#include "defs.h"
#include "settings.h"

#define EEPROM_SIZE     256
#define SETTINGS_MAGIC  0xC5    // anything else at ADDR_MAGIC - nothing stored yet (erased flash is 0xff)

#define ADDR_MAGIC      0
#define ADDR_FLIPPED    1

static bool flipped;

void settingsInit(void)
{
    EEPROM.begin(EEPROM_SIZE);

    if(EEPROM.read(ADDR_MAGIC) == SETTINGS_MAGIC) {
        flipped = EEPROM.read(ADDR_FLIPPED) != 0;
    } else {
        flipped = DEFAULT_FLIPPED;
    }
}

bool settingsFlipped(void)
{
    return flipped;
}

void settingsSetFlipped(bool value)
{
    flipped = value;

    EEPROM.write(ADDR_MAGIC, SETTINGS_MAGIC);
    EEPROM.write(ADDR_FLIPPED, flipped ? 1 : 0);
    EEPROM.commit();
}
