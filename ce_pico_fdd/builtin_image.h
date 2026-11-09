#ifndef __BUILTIN_IMAGE_H__
#define __BUILTIN_IMAGE_H__

#include <cstdint>

// Built-in floppy: served from the start until the first real image from the host is loaded completely - before there are
// WiFi settings, while connecting, while loading the image, and while the host has no image in the drive. Also again
// when the WiFi setup (captive portal) starts later - until the image is loaded again after it.
//
// Single sided 80 x 9, write protected (writes to it are dropped on core 1), FAT12 with:
// - executable boot sector and A:\CE_FDD.TOS: show the device status on the ST, refreshed once a second
//   (builtin_floppy/status.s) until SPACE is pressed, 'ready, reset your ST' after the switch to the real image
// - a text file with the status, named after it (SETUP.TXT, NO_WIFI.TXT, WIFI.TXT, HOST.TXT, LOADING.TXT, NO_IMAGE.TXT)
// - track 0 sector 10 - outside of the file system - with the status for the boot code: 'CE2XSTAT', code, change counter,
//   text from offset 16
// The disk content is made by builtin_floppy/make_builtin.py (builtin_floppy_data.h). The tracks are made here on core 0
// and sent to core 1 like received tracks (CMD_STORE_BUILTIN_TRACK), which keeps them in their own PSRAM area - the
// real image is received into its usual area meanwhile, and core 1 switches to it on CMD_SHOW_REAL_IMAGE.

#define BUILTIN_STATUS_NONE         0
#define BUILTIN_STATUS_NO_WIFI      1   // no WiFi settings, the WiFi setup didn't start (yet)
#define BUILTIN_STATUS_WIFI         2   // joining the WiFi
#define BUILTIN_STATUS_HOST         3   // looking for the host / connecting to it
#define BUILTIN_STATUS_LOADING      4   // loading the image
#define BUILTIN_STATUS_NO_IMAGE     5   // connected, but the host has no image in the drive
#define BUILTIN_STATUS_WIFI_SETUP   6   // the WiFi setup (captive portal) is running - open the setup page

void builtinImageStart(void);           // setup(), after core 1 runs: make the whole floppy before the ST boots
void builtinImageLoop(void);            // core 0 main loop: make the tracks, follow the status, switch to the real image
bool builtinImageActive(void);          // the ST sees the built-in floppy (core 0 view - core 1 switches a moment later)
void builtinImageRealImageLoaded(void); // the host sent a whole real image - switch to it, for good
void builtinImageReset(void);           // back to the start state - for the tests, the firmware starts in it

#endif
