// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/hdd_if.h"
#include "../libacsiscsi/stdlib.h"
#include "../libacsiscsi/translated.h"
#include "video.h"
#include "brw.h"

uint8_t *dmaBuf;
uint16_t dmaBufSectors;

static uint8_t deviceId;

// ICD long command: [0] ID + 0x1f, [1] 0xa0, [2..3] 'CE', [4] module, [5] command, [6..12] args = host's pCmd[5..11]
static uint8_t cmdLong[CMD_LENGTH_LONG] = { 0x1f, 0xa0, 'C', 'E', HOSTMOD_BROWSER, 0, 0, 0, 0, 0, 0, 0, 0 };

uint16_t getWordBE(const uint8_t *p)
{
    return ((uint16_t) p[0] << 8) | p[1];
}

uint32_t getDwordBE(const uint8_t *p)
{
    return ((uint32_t) getWordBE(p) << 16) | getWordBE(p + 2);
}

uint8_t brwInit(void)
{
    // TT: 254 sectors - a whole TT frame in 2 transfers. ST: 64 sectors (32 kB) - ST frames fit, and 1 MB machines
    // keep their memory, and the bus isn't held long.
    dmaBufSectors = (machineType >= MACHINE_TYPE_TT) ? MAXSECTORS : 64;
    uint32_t size = (uint32_t) dmaBufSectors * 512 + 16;

    uint8_t *raw;
    if(machineType >= MACHINE_TYPE_TT) {
        raw = (uint8_t *) Mxalloc(size, 0);                         // ST RAM - ACSI / SCSI DMA can't reach TT RAM
    } else {
        raw = (uint8_t *) Malloc(size);
    }
    if(!raw || ((int32_t) raw) < 0) {
        return FALSE;
    }
    dmaBuf = (uint8_t *) ((((uint32_t) raw) + 15) & 0xfffffff0);   // longword - TT SCSI DMA needs it
    return TRUE;
}

static uint8_t cmd(uint8_t readNotWrite, uint8_t c, const uint8_t *args, uint8_t nargs, uint16_t sectors)
{
    uint8_t i;

    cmdLong[0] = (deviceId << 5) | 0x1f;
    cmdLong[5] = c;
    for(i=0; i<7; i++) {
        cmdLong[6 + i] = (i < nargs) ? args[i] : 0;
    }

    hdIfCmdAsUser(readNotWrite, cmdLong, CMD_LENGTH_LONG, dmaBuf, sectors);
    if(!hdIf.success) {
        return BRW_FAILED;
    }
    return hdIf.statusByte;
}

static uint8_t identify(uint8_t id)
{
    deviceId = id;
    memset(dmaBuf, 0, 512);
    if(cmd(ACSI_READ, BRW_CMD_IDENTIFY, NULL, 0, 1) != BRW_OK) {
        return FALSE;
    }
    // a device without the module may answer like to TEST UNIT READY - status OK, no data
    return strncmp((char *) dmaBuf, "CosmosEx browser module", 23) == 0;
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

static uint32_t cookieVal;
static void readCepiSuper(void)
{
    uint32_t *jar = *((uint32_t **) 0x5a0);
    cookieVal = 0;
    for(; jar && jar[0] != 0; jar += 2) {
        if(jar[0] == 0x43455049UL) {                                // 'CEPI' - CE_DD's hdIf
            cookieVal = jar[1];
            return;
        }
    }
}

uint8_t brwFind(void)
{
    uint8_t found = FALSE;

    hdIf.maxRetriesCount = 0;                                       // other IDs won't answer - don't retry them

    Supexec(readCepiSuper);                                         // the device CE_DD uses first
    THDif *ceHdIf = (THDif *) cookieVal;
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
        switch(machineType) {
            case MACHINE_TYPE_TT:       found = scanIf(IF_ACSI) || scanIf(IF_SCSI_TT);  break;
            case MACHINE_TYPE_FALCON:   found = scanIf(IF_SCSI_FALCON);                 break;
            default:                    found = scanIf(IF_ACSI);                        break;
        }
    }

    hdIf.maxRetriesCount = 4;
    return found;
}

//--------------------------------------------------
uint8_t brwSetMode(uint8_t res, uint16_t pageH, uint8_t zoomQ)
{
    uint8_t a[4] = { res, pageH >> 8, pageH & 0xff, zoomQ };
    return cmd(ACSI_READ, BRW_CMD_SET_MODE, a, 4, 1);
}

uint8_t brwNavigate(const char *url)
{
    memset(dmaBuf, 0, BRW_URL_SIZE);
    strncpy((char *) dmaBuf, url, BRW_URL_SIZE - 1);
    return cmd(ACSI_WRITE, BRW_CMD_NAVIGATE, NULL, 0, BRW_URL_SIZE / 512);
}

uint8_t brwNavAction(uint8_t action)
{
    return cmd(ACSI_READ, BRW_CMD_NAV_ACTION, &action, 1, 1);
}

uint8_t brwMouse(uint8_t event, uint16_t x, uint16_t y, uint8_t buttons)
{
    uint8_t a[6] = { event, x >> 8, x & 0xff, y >> 8, y & 0xff, buttons };
    return cmd(ACSI_READ, BRW_CMD_MOUSE, a, 6, 1);
}

uint8_t brwKey(uint8_t scancode, uint8_t ascii, uint8_t shift)
{
    uint8_t a[3] = { scancode, ascii, shift };
    return cmd(ACSI_READ, BRW_CMD_KEY, a, 3, 1);
}

uint8_t brwScroll(int8_t lines, int8_t pages, int8_t cols, int8_t hpages)
{
    uint8_t a[4] = { (uint8_t) lines, (uint8_t) pages, (uint8_t) cols, (uint8_t) hpages };
    return cmd(ACSI_READ, BRW_CMD_SCROLL, a, 4, 1);
}

uint8_t brwGetUpdate(uint32_t baseSeq, uint16_t resumeRow)
{
    uint8_t a[7] = { baseSeq >> 24, baseSeq >> 16, baseSeq >> 8, baseSeq & 0xff, (uint8_t) dmaBufSectors,
                     resumeRow >> 8, resumeRow & 0xff };
    return cmd(ACSI_READ, BRW_CMD_GET_UPDATE, a, 7, dmaBufSectors);
}

uint8_t brwGetStatus(void)
{
    memset(dmaBuf, 0, BRW_STATUS_SIZE);
    return cmd(ACSI_READ, BRW_CMD_GET_STATUS, NULL, 0, BRW_STATUS_SIZE / 512);
}

uint8_t brwGetHistory(uint8_t maxCount)
{
    uint8_t a[2] = { maxCount, (uint8_t) dmaBufSectors };
    dmaBuf[0] = dmaBuf[1] = 0;
    return cmd(ACSI_READ, BRW_CMD_GET_HISTORY, a, 2, dmaBufSectors);
}
