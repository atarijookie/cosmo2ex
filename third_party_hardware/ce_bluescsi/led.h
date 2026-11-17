#ifndef __LED_H__
#define __LED_H__

#include <Arduino.h>

// Status on the LED of the pico module (Pico W / 2W: the LED is on the WiFi chip, so only core 0 drives it):
// - blinking fast (100 ms on, 400 ms off) - starting, connecting to WiFi / host
// - on                                     - connected to the host
// - N short blinks, then a pause           - error N (LED_ERR_*)
// The BlueSCSI board has its own activity LED - it's lit while BSY is asserted (by anyone).

#define LED_STEP_SETUP          1       // core 0 setup started
#define LED_STEP_CORE1          2       // core 1 (bus handling) running
#define LED_STEP_NETWORK        3       // joining WiFi / waiting for IP
#define LED_STEP_GOT_IP         4       // got IP
#define LED_STEP_CONNECTING     5       // in main loop, looking for / connecting to host

#define LED_ERR_NO_NET_HW       1       // network chip not working (halted)
#define LED_ERR_NO_LINK         2       // can't join the WiFi network (keeps retrying)
#define LED_ERR_NO_DHCP         3       // no IP from DHCP (keeps retrying)
#define LED_ERR_PIO             4       // failed to claim PIO state machine (halted)
#define LED_ERR_NO_WIFI_CONFIG  5       // no WiFi SSID - secrets.h missing (halted, captive portal later)

// setting the status - from any core
void ledSetInitStep(uint8_t step);      // also clears the error
void ledSetError(uint8_t code);
void ledSetRunning(void);
void haltWithError(uint8_t code);       // show the error and stop this core forever

// output - core 0 only
void ledInit(void);
void ledProcess(void);                  // from the core 0 loop

#endif
