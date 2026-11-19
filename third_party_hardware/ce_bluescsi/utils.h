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

#define SETTINGS_MAGIC  0xb1e5c510    // not the one of ce_pico_hdd - different layout

struct __attribute__((packed)) TSettings
{
    uint32_t magic;
    uint8_t mac[6];         // for the W6100
    uint8_t enabledIDs;     // from the host (web UI)
};

extern TSettings settings;
void loadSettings(void);
void makeMacFromBoardId(uint8_t *mac);
void saveSettings(void);    // core 0 - the arduino core idles core 1 meanwhile (it's started via setup1() / loop1())

void debugInit(void);       // core 0, after boardDetect()
void debug(const char *fmt, ...);

#ifdef LOG_PIO
void debugFromQueue(void);  // core 0 - sends what core 1 queued
#endif

#endif
