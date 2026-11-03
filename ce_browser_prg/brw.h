#ifndef _BRW_H_
#define _BRW_H_

// Commands of the CosmosEx browser module (HOSTMOD_BROWSER) - ../ce_cores/hdd/browser/browser_commands.h.
// Every reply lands in dmaBuf (ST RAM, aligned as the interface needs).

#include <stdint.h>
#include "../ce_cores/hdd/browser/browser_commands.h"

#define BRW_FAILED      0xff        // the device didn't answer (hdIf.success false)

extern uint8_t *dmaBuf;
extern uint16_t dmaBufSectors;      // max sectors of one reply

uint8_t brwInit(void);              // allocate the DMA buffer; FALSE = no memory
uint8_t brwFind(void);              // find the device with the browser module (also starts ce_browser on the host)

uint8_t brwSetMode(uint8_t res, uint16_t pageH, uint8_t zoomQ);
uint8_t brwNavigate(const char *url);
uint8_t brwNavAction(uint8_t action);
uint8_t brwMouse(uint8_t event, uint16_t x, uint16_t y, uint8_t buttons);
uint8_t brwKey(uint8_t scancode, uint8_t ascii, uint8_t shift);
uint8_t brwScroll(int8_t lines, int8_t pages, int8_t cols, int8_t hpages);
uint8_t brwGetUpdate(uint32_t baseSeq, uint16_t resumeRow);
uint8_t brwGetStatus(void);
uint8_t brwGetHistory(uint8_t maxCount);

uint16_t getWordBE(const uint8_t *p);
uint32_t getDwordBE(const uint8_t *p);

#endif
