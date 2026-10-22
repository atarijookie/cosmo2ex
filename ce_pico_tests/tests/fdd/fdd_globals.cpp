// Globals that the firmware defines in ce_pico_fdd_vsc.cpp - that file (main, WiFi, lwIP) is not compiled into the tests,
// but the tested sources (core1_main.cpp, mfm_write.cpp, psram.cpp, host_receive.cpp) use these.

#include "defs.h"

TDrivePosition posStreamed, hwPosition;

uint8_t trackData0[READTRACKDATA_SIZE_BYTES];
uint8_t trackData1[READTRACKDATA_SIZE_BYTES];

uint8_t imgTracks = 80, imgSides = 2, imgSectorsPerTrack = 9;

int imageState = IMAGE_NOT_LOADED;
char imageFileName[32];
bool diskChanged = false;
