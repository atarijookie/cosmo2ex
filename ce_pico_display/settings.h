#ifndef SETTINGS_H_
#define SETTINGS_H_

// panel settings in the emulated EEPROM (the last flash sector)

void settingsInit(void);

bool settingsFlipped(void);
void settingsSetFlipped(bool flipped);  // stored right away - the flash write stops the panel for a moment

#endif
