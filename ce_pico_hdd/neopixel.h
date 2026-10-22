#ifndef __NEOPIXEL_H__
#define __NEOPIXEL_H__

#include <Arduino.h>

// Status shown on the neopixels (LED2 top, LED3 bottom - both get the same data):
// - orange during init, brighter with each step (LED_STEP_*)
// - green once connected to the host
// - red blinks with the error code (count of blinks, then a longer pause) on error (LED_ERR_*)

#define LED_STEP_SETUP          1       // core 0 setup started
#define LED_STEP_CORE1          2       // core 1 (bus handling) running
#define LED_STEP_ETHERNET       3       // waiting for link + IP from DHCP
#define LED_STEP_GOT_IP         4       // got IP
#define LED_STEP_CONNECTING     5       // in main loop, looking for / connecting to host
#define LED_STEP_COUNT          5

#define LED_ERR_NO_ETHERNET     1       // ethernet chip not found (halted)
#define LED_ERR_NO_LINK         2       // ethernet cable not connected (keeps retrying)
#define LED_ERR_NO_DHCP         3       // no IP from DHCP (keeps retrying)
#define LED_ERR_PIO             4       // failed to claim PIO state machine (halted)

#define NEOPIXEL_RESET_US       300     // L time needed for latching the color, 280 us for the newer WS2812 parts

// setting the status - from any core
void ledSetInitStep(uint8_t step);      // also clears the error
void ledSetError(uint8_t code);
void ledSetRunning(void);
void haltWithError(uint8_t code);       // show the error and stop this core forever

// output - on core 1, only while the bus is idle (the neopixel pin is shared with SCSI signals)
void neopixelInit(void);
void neopixelProcess(void);             // send the current color when changed, during init / error also once per second
void neopixelSendFrame(uint32_t grb);   // just the 24 bits, the pin must be an SIO output driven L

#endif
