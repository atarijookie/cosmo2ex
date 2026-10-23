#ifndef __CAPTIVE_PORTAL_H__
#define __CAPTIVE_PORTAL_H__

// WiFi setup: open AP 'cosmo2ex', every DNS name and URL leads to a page with the WiFi name + password, which are
// saved to EEPROM, then the FDD goes back to the normal (station) mode and connects with them.
// arduino-pico can't be AP and station at once - the host connection is down while the portal runs (core 1 keeps
// streaming the current image).

#define PORTAL_AP_NAME      "cosmo2ex"
#define PORTAL_TIMEOUT_MS   (5 * 60 * 1000)     // back to the old settings after this long without a page request
#define PORTAL_SAVED_MS     1000                // after saving, keep the AP this long, so the phone gets the answer
#define PORTAL_RETRY_MS     10000               // no WiFi settings and the AP failed to start - try again after this

void startCaptivePortal(void);
void stopCaptivePortal(void);       // back to station mode, with whatever settings are stored
void toggleCaptivePortal(void);     // long press: start, or leave without saving (not without WiFi settings)
bool captivePortalActive(void);
void captivePortalLoop(void);       // call from the main loop instead of connectToHost() while active
void captivePortalAutoStart(void);  // call from the main loop - with no WiFi settings stored the portal starts, and
                                    // stays (no timeout, the long press doesn't leave it) until settings are saved

#endif
