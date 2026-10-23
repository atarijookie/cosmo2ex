#ifndef __NEOPIXEL_H__
#define __NEOPIXEL_H__

#include <cstdint>

// Status shown on the neopixel (LED2), like on ce_pico_hdd:
// - orange during init, brighter with each step (LED_STEP_*)
// - green when connected to the host and an image is loaded, dim white when connected without an image
// - blue while the captive portal (WiFi setup) runs
// - red blinks with the error code (count of blinks, then a longer pause) on error (LED_ERR_*)
// The color is sent only when it changes - nothing is sent while the state stays the same (and the blinks only while
// there is an error). All on core 0, so the floppy streaming on core 1 doesn't lose any cycles to it.

#define LED_STEP_SETUP          1       // core 0 setup started
#define LED_STEP_CORE1          2       // core 1 (floppy streaming) running, PSRAM OK
#define LED_STEP_WIFI           3       // joining the WiFi network
#define LED_STEP_DISCOVERY      4       // on WiFi, looking for the host
#define LED_STEP_CONNECTING     5       // host found, connecting to it
#define LED_STEP_COUNT          5

#define LED_ERR_PSRAM           1       // PSRAM missing or not working (halted)
#define LED_ERR_NO_SSID         2       // no WiFi settings stored and the captive portal failed to start (retried)
#define LED_ERR_NO_WIFI         3       // can't join the WiFi network (keeps retrying)

#define NEOPIXEL_GAP_MS         2       // at least this long between frames: L for 280+ us latches the color

// setting the status - from any core
void ledSetInitStep(uint8_t step);      // also clears the error
void ledSetError(uint8_t code);
void ledSetRunning(bool imageLoaded);
void ledSetCaptivePortal(void);

// the status from the connection state - called from the core 0 main loop, outside of the captive portal
void ledSetFromConnection(bool ssidStored, bool wifiFailed, bool wifiConnected, bool hostFound, bool hostConnected,
                          bool imageLoaded);

// output - on core 0
void neopixelInit(void);
void neopixelProcess(void);             // send the current color if it changed
void neopixelResend(void);              // send the color again on the next neopixelProcess() - after a button press,
                                        // which the LED saw as a long H on its data input

// the frame itself - neopixel_pio.cpp (faked in the tests)
void neopixelOutputInit(void);
void neopixelOutput(uint32_t grb);

#endif
