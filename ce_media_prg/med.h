#ifndef _MED_H_
#define _MED_H_

// Commands of the CosmosEx media module (HOSTMOD_MEDIA_STREAMING) - ../ce_cores/hdd/media/media_commands.h.

#include <stdint.h>
#include "../ce_cores/hdd/media/media_commands.h"

#define MED_FAILED      0xff        // the device didn't answer (hdIf.success false)

extern uint8_t *medBuf;             // small replies land here (ST RAM, 512 B aligned, 8 kB)
extern uint8_t  medKeepFlock;       // plain ST with YM sound: keep the floppy VBL away from the YM between commands

uint8_t medInit(uint8_t machine);   // FALSE = no memory
uint8_t medFind(uint8_t machine);   // find the device with the media module (also starts ce_media on the host)

uint8_t medOpen(const uint8_t *openHeader, const char *atariPath);
uint8_t medGetInfo(void);                                   // -> medBuf
uint8_t medGetYmTable(void);                                // -> medBuf
uint8_t medClose(void);
uint8_t medGetChunk(uint8_t *buf, uint8_t sectors, uint32_t generation);   // buf: 512 B aligned, ST RAM
uint8_t medSeek(uint32_t posMs);                            // -> medBuf: new generation, position
uint8_t medInput(const uint8_t *codes, uint8_t count);

uint16_t getWordBE(const uint8_t *p);
uint32_t getDwordBE(const uint8_t *p);
void     putWordBE(uint8_t *p, uint16_t v);
void     putDwordBE(uint8_t *p, uint32_t v);

uint32_t readCookie(uint32_t id);   // 0 when not there

#endif
