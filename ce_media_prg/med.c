// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/hdd_if.h"
#include "../libacsiscsi/stdlib.h"
#include "../libacsiscsi/translated.h"
#include "med.h"

uint8_t *medBuf;
uint8_t  medKeepFlock;

#define MED_BUF_SECTORS     (MED_YM_TABLE_SIZE / 512)

static uint8_t deviceId;

// ICD long command: [0] ID + 0x1f, [1] 0xa0, [2..3] 'CE', [4] module, [5] command, [6..12] args = host's pCmd[5..11]
static uint8_t cmdLong[CMD_LENGTH_LONG] = { 0x1f, 0xa0, 'C', 'E', HOSTMOD_MEDIA_STREAMING, 0, 0, 0, 0, 0, 0, 0, 0 };

uint16_t getWordBE(const uint8_t *p)
{
    return ((uint16_t) p[0] << 8) | p[1];
}

uint32_t getDwordBE(const uint8_t *p)
{
    return ((uint32_t) getWordBE(p) << 16) | getWordBE(p + 2);
}

void putWordBE(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}

void putDwordBE(uint8_t *p, uint32_t v)
{
    putWordBE(p, v >> 16);
    putWordBE(p + 2, v);
}

static uint32_t cookieId, cookieValue;
static void readCookieSuper(void)
{
    uint32_t *jar = *((uint32_t **) 0x5a0);
    cookieValue = 0;
    for(; jar && jar[0] != 0; jar += 2) {
        if(jar[0] == cookieId) {
            cookieValue = jar[1];
            return;
        }
    }
}

uint32_t readCookie(uint32_t id)
{
    cookieId = id;
    Supexec(readCookieSuper);
    return cookieValue;
}

uint8_t medInit(uint8_t machine)
{
    uint32_t size = (uint32_t) MED_BUF_SECTORS * 512 + 512;
    uint8_t *raw;
    if(machine >= MED_MACH_TT) {
        raw = (uint8_t *) Mxalloc(size, 0);                         // ST RAM - ACSI / SCSI DMA can't reach TT RAM
    } else {
        raw = (uint8_t *) Malloc(size);
    }
    if(!raw || ((int32_t) raw) < 0) {
        return FALSE;
    }
    medBuf = (uint8_t *) ((((uint32_t) raw) + 511) & 0xfffffe00);
    return TRUE;
}

static void setFlockSuper(void)
{
    *((volatile int16_t *) 0x43e) = -1;
}

static uint8_t cmd(uint8_t readNotWrite, uint8_t c, const uint8_t *args, uint8_t nargs, uint8_t *buf, uint16_t sectors)
{
    uint8_t i;

    cmdLong[0] = (deviceId << 5) | 0x1f;
    cmdLong[5] = c;
    for(i=0; i<7; i++) {
        cmdLong[6 + i] = (i < nargs) ? args[i] : 0;
    }

    hdIfCmdAsUser(readNotWrite, cmdLong, CMD_LENGTH_LONG, buf, sectors);
    if(medKeepFlock) {                                              // the command released it
        Supexec(setFlockSuper);
    }
    if(!hdIf.success) {
        return MED_FAILED;
    }
    return hdIf.statusByte;
}

static uint8_t identify(uint8_t id)
{
    deviceId = id;
    memset(medBuf, 0, 512);
    if(cmd(ACSI_READ, MED_CMD_IDENTIFY, NULL, 0, medBuf, 1) != MED_OK) {
        return FALSE;
    }
    return strncmp((char *) medBuf, "CosmosEx media module", 21) == 0;
}

static uint8_t scanIf(int ifType)
{
    uint8_t id;

    hdd_if_select(ifType);
    for(id = 0; id < 8; id++) {
        if(id != hdIf.scsiHostId && identify(id)) {
            return TRUE;
        }
    }
    return FALSE;
}

uint8_t medFind(uint8_t machine)
{
    uint8_t found = FALSE;

    hdIf.maxRetriesCount = 0;                                       // other IDs won't answer - don't retry them

    THDif *ceHdIf = (THDif *) readCookie(0x43455049UL);             // 'CEPI' - the device CE_DD uses
    if(ceHdIf && ceHdIf->ceDeviceId <= 7) {
        int ifType = -1;
        switch(ceHdIf->scsiHostId) {
            case 0xff:  ifType = IF_ACSI;           break;
            case 7:     ifType = IF_SCSI_TT;        break;
            case 0:     ifType = IF_SCSI_FALCON;    break;
        }
        if(ifType >= 0) {
            hdd_if_select(ifType);
            found = identify(ceHdIf->ceDeviceId);
        }
    }

    if(!found) {
        switch(machine) {
            case MED_MACH_TT:       found = scanIf(IF_ACSI) || scanIf(IF_SCSI_TT);  break;
            case MED_MACH_FALCON:   found = scanIf(IF_SCSI_FALCON);                 break;
            default:                found = scanIf(IF_ACSI);                        break;
        }
    }

    hdIf.maxRetriesCount = 4;
    return found;
}

//--------------------------------------------------
uint8_t medOpen(const uint8_t *openHeader, const char *atariPath)
{
    memset(medBuf, 0, MED_OPEN_SIZE);
    memcpy(medBuf, openHeader, MED_OPEN_PATH);
    strncpy((char *) medBuf + MED_OPEN_PATH, atariPath, MED_OPEN_PATH_SIZE - 1);
    return cmd(ACSI_WRITE, MED_CMD_OPEN, NULL, 0, medBuf, MED_OPEN_SIZE / 512);
}

uint8_t medGetInfo(void)
{
    memset(medBuf, 0, MED_INFO_SIZE);
    return cmd(ACSI_READ, MED_CMD_GET_INFO, NULL, 0, medBuf, MED_INFO_SIZE / 512);
}

uint8_t medGetYmTable(void)
{
    return cmd(ACSI_READ, MED_CMD_GET_YM_TABLE, NULL, 0, medBuf, MED_YM_TABLE_SIZE / 512);
}

uint8_t medClose(void)
{
    return cmd(ACSI_READ, MED_CMD_CLOSE, NULL, 0, medBuf, 1);
}

uint8_t medGetChunk(uint8_t *buf, uint8_t sectors, uint32_t generation)
{
    uint8_t a[5] = { sectors, generation >> 24, generation >> 16, generation >> 8, generation & 0xff };
    buf[MED_CHUNK_FLAGS] = MED_CHF_EMPTY;                           // if the device sends nothing
    return cmd(ACSI_READ, MED_CMD_GET_CHUNK, a, 5, buf, sectors);
}

uint8_t medSeek(uint32_t posMs)
{
    uint8_t a[4] = { posMs >> 24, posMs >> 16, posMs >> 8, posMs & 0xff };
    return cmd(ACSI_READ, MED_CMD_SEEK, a, 4, medBuf, 1);
}

uint8_t medInput(const uint8_t *codes, uint8_t count)
{
    uint8_t a[7];
    uint8_t i;
    if(count > 6) {
        count = 6;
    }
    a[0] = count;
    for(i=0; i<6; i++) {
        a[1 + i] = (i < count) ? codes[i] : 0;
    }
    return cmd(ACSI_READ, MED_CMD_INPUT, a, 7, medBuf, 1);
}
