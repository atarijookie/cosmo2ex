// WiFi credentials for ce_bluescsi - copy this file to secrets.h (same folder) and fill it in.
// secrets.h is in .gitignore - never commit it. firmware/compile.sh (docker / release builds) deletes it from its copy of
// the sources, so the released .uf2 never carries anybody's WiFi password.
//
// Temporary - the captive portal (SW1 on the BlueSCSI board) will replace this.

#define WIFI_SSID       "your-network"
#define WIFI_PASSWORD   "your-password"
