#include <mint/osbind.h>
#include <stdint.h>

#include "acsi.h"
#include "hdd_if.h"
#include "stdlib.h"

#include "cenet.h"

void clearCache030(void);

static uint8 deviceId;
static uint8 cpu030;            // has a data cache, which doesn't see DMA writes to RAM

// ICD10: [0] ID + 0x1F, [1] 0x20, [2..3] 'CE', [4] HOSTMOD (+ 0x80 on retry), [5] function, [6..10] parameters
static uint8 cmdLong[11]  = { 0x1f, 0x20, 'C', 'E', HOSTMOD_RAW_NET, 0, 0, 0, 0, 0, 0 };
// SCSI(6): [0] ID + 0 (TEST UNIT READY), [1..2] 'CE', [3] HOSTMOD (+ 0x80 on retry), [4] function, [5] parameter
static uint8 cmdShort[6]  = { 0x00, 'C', 'E', HOSTMOD_RAW_NET, 0, 0 };

static uint8 identBufMem[512 + 4];
static uint8 *dmaBuffer;        // used instead of identBufMem when set

static int doCmd(uint8 readNotWrite, uint8 *cmd, uint8 cmdLen, uint8 *buf, uint16 sectors)
{
    cmd[0] = (cmd[0] & 0x1f) | (deviceId << 5);

    (*hdIf.cmd_nolock)(readNotWrite, cmd, cmdLen, buf, sectors);

    if(readNotWrite == ACSI_READ && cpu030) {
        clearCache030();
    }

    if(!hdIf.success) {
        return -1;
    }

    return hdIf.statusByte;
}

void netSetDmaBuffer(uint8 *buf)
{
    dmaBuffer = buf;
}

static uint8 *identBuf(void)
{
    if(dmaBuffer) {
        return dmaBuffer;
    }

    return (uint8 *) (((uint32) identBufMem + 3) & ~3UL);     // longword aligned - TT SCSI DMA
}

static uint8 isOurDevice(uint8 id)
{
    deviceId = id;

    uint8 *buf = identBuf();
    memset(buf, 0, 32);

    cmdShort[4] = RAWNET_CMD_IDENTIFY;
    int status = doCmd(ACSI_READ, cmdShort, 6, buf, 1);

    // Firmware without this answers as to an ordinary TEST UNIT READY - status OK, but no data. So check the data.
    return (status == RAWNET_ST_OK && memcmp(buf, "CERAWNET", 8) == 0);
}

static uint8 scanIf(int ifType)
{
    hdd_if_select(ifType);

    for(uint8 id = 0; id < 8; id++) {
        if(id == hdIf.scsiHostId) {
            continue;
        }

        if(isOurDevice(id)) {
            return TRUE;
        }
    }

    return FALSE;
}

uint32 readCookie(uint32 id)
{
    uint32 *jar = *((uint32 **) 0x5a0);

    for(; jar && jar[0] != 0; jar += 2) {
        if(jar[0] == id) {
            return jar[1];
        }
    }
    return 0;
}

// try the ID CE_DD uses - it publishes its hdIf in the CEPI cookie
static uint8 tryCeDdDevice(void)
{
    THDif *ceHdIf = (THDif *) readCookie(0x43455049UL);     // 'CEPI'
    if(!ceHdIf) {
        return FALSE;
    }

    uint8 id = ceHdIf->ceDeviceId;          // older CE_DD doesn't have it - whatever is there gets checked by IDENTIFY
    if(id > 7) {
        return FALSE;
    }

    int ifType;
    switch(ceHdIf->scsiHostId) {            // which interface CE_DD uses
        case 0xff:  ifType = IF_ACSI;           break;
        case 7:     ifType = IF_SCSI_TT;        break;
        case 0:     ifType = IF_SCSI_FALCON;    break;
        default:    return FALSE;
    }

    hdd_if_select(ifType);
    return isOurDevice(id);
}

uint8 netFindDevice(void)
{
    cpu030 = (readCookie(0x5F435055UL) >= 20);   // '_CPU' - 20 and up have caches

    hdIf.maxRetriesCount = 0;               // devices on the other IDs won't answer - don't retry

    uint8 found = tryCeDdDevice();

    if(found) {
        hdIf.maxRetriesCount = 1;
        return TRUE;
    }

    switch(getMachineType()) {             // no CE_DD (or it has another device) - look on the buses
        case MACHINE_TT:
            found = scanIf(IF_ACSI) || scanIf(IF_SCSI_TT);
            break;

        case MACHINE_FALCON:
            found = scanIf(IF_SCSI_FALCON);
            break;

        default:
            found = scanIf(IF_ACSI);
            break;
    }

    hdIf.maxRetriesCount = 1;               // a failed transfer gets one more try, more would stall STinG for too long
    return found;
}

int netIdentify(TNetInfo *info)
{
    uint8 *buf = identBuf();
    memset(buf, 0, 32);

    cmdShort[4] = RAWNET_CMD_IDENTIFY;
    int status = doCmd(ACSI_READ, cmdShort, 6, buf, 1);

    if(status != RAWNET_ST_OK || memcmp(buf, "CERAWNET", 8) != 0) {
        return -1;
    }

    info->version   = (buf[8] << 8) | buf[9];
    memcpy(info->mac, buf + 10, 6);
    info->link      = buf[16];
    info->maxBatch  = (buf[18] << 8) | buf[19];
    return status;
}

int netConfig(uint8 flags, uint32 ip)
{
    cmdLong[5]  = RAWNET_CMD_CONFIG;
    cmdLong[6]  = flags;
    cmdLong[7]  = ip >> 24;
    cmdLong[8]  = ip >> 16;
    cmdLong[9]  = ip >>  8;
    cmdLong[10] = ip;

    return doCmd(ACSI_READ, cmdLong, 11, identBuf(), 1);       // no data phase, just status
}

int netSend(uint8 *buf, uint32 len, uint8 batchNo, uint8 pktCount)
{
    cmdLong[5]  = RAWNET_CMD_SEND;
    cmdLong[6]  = len >> 16;
    cmdLong[7]  = len >>  8;
    cmdLong[8]  = len;
    cmdLong[9]  = batchNo;
    cmdLong[10] = pktCount;

    return doCmd(ACSI_WRITE, cmdLong, 11, buf, (len + 511) >> 9);
}

int netRecv(uint8 *buf, uint8 maxSectors, uint8 batchNo)
{
    cmdLong[5]  = RAWNET_CMD_RECV;
    cmdLong[6]  = maxSectors;
    cmdLong[7]  = batchNo;
    cmdLong[8]  = 0;
    cmdLong[9]  = 0;
    cmdLong[10] = 0;

    return doCmd(ACSI_READ, cmdLong, 11, buf, maxSectors);
}
