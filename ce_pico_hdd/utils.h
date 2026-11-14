#ifndef __UTILS_H__
#define __UTILS_H__

#include <Arduino.h>
#include "defs.h"

uint16_t getWord(uint8_t *bfr);
uint32_t getDword(uint8_t *bfr);
uint32_t get24bits(uint8_t *bfr);
void storeWord(uint8_t *bfr, uint16_t val);
void storeDword(uint8_t *bfr, uint32_t val);
void store24bits(uint8_t *bfr, uint32_t val);
void storeHeader(uint8_t *bfr, uint16_t atnCode, uint32_t txLen);

extern uint32_t timerEndMillis;
extern volatile bool hasTimedOut;
void timeoutClear(void);
void timeoutStart(uint32_t durationMs = CMD_TIMEOUT_SHORT);
void longTimeout_basedOnSectorCount(uint16_t sectorCount);
void cmdTimeoutChangeLength(uint32_t newPeriod);

#define SETTINGS_MAGIC  0xcafebabe

struct __attribute__((packed)) TSettings
{
    uint32_t magic;
    uint8_t mac[6];
    uint8_t enabledIDs;
    uint8_t ikbdEnabled;
    uint8_t ledAcsi[3];     // neopixel R, G, B while running on ACSI - from the host (web UI)
    uint8_t ledScsi[3];     // the same on SCSI
    uint8_t ledCheck;       // LED_COLORS_CHECK ^ the 6 bytes above - not matching: not stored yet (new board, older
                            // firmware left garbage there), the defaults are used
};

#define LED_COLORS_CHECK    0xA5

extern TSettings settings;
void loadSettings(void);
void makeMacFromBoardId(uint8_t *mac);
void saveSettings(void);
void ledColorsReceived(const uint8_t* acsiRgb, const uint8_t* scsiRgb);    // from the host: apply, store if changed

void debugInit(void);
void debug(const char *fmt, ...);

#ifdef LOG_LED
void debugFromQueue(void);
#endif

void dump_gpio(uint gpio);

#endif
