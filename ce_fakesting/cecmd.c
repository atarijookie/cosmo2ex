// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <stdint.h>

#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/hdd_if.h"
#include "../libacsiscsi/stdlib.h"
#include "globdefs.h"
#include "global.h"
#include "ce_commands.h"
#include "stdlib2.h"
#include "vbl.h"
#include "cecmd.h"

extern uint8_t  deviceID;
extern uint32_t localIP;
extern uint16_t requiredVersion;
extern uint16_t netHandlesCount;

void ceCmd(uint8_t readNotWrite, uint8_t *cmd, uint8_t cmdLength, uint8_t *buffer, uint16_t sectorCount)
{
    if(fromVbl) {                                   // VBL - already in supervisor
        (*hdIf.cmd)(readNotWrite, cmd, cmdLength, buffer, sectorCount);
    } else {                                        // STinG API call from the application - user mode (or not, Supexec() works in both)
        hdIfCmdAsUser(readNotWrite, cmd, cmdLength, buffer, sectorCount);
    }
}

uint32_t ceTicks(void)
{
    return fromVbl ? getTicks() : getTicksAsUser();
}

uint8_t ceNeedsBounce(const void *buffer)
{
    uint32_t addr = (uint32_t) buffer;

    if(addr >= 0x01000000) {                        // TT fast RAM - ACSI / SCSI DMA reaches only ST RAM
        return TRUE;
    }

    return (addr & hdIf.dmaAlignMask) != 0;
}

//--------------------------------------------------
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

static uint32_t readCookie(uint32_t id)
{
    cookieId = id;
    Supexec(readCookieSuper);
    return cookieValue;
}

// is the network module on this ID? Reads the version and the local IP too.
static uint8_t netIdentify(uint8_t id)
{
    uint8_t cmd[CMD_LENGTH_SHORT] = { 0, 'C', 'E', HOSTMOD_NETWORK_ADAPTER, NET_CMD_IDENTIFY, 0 };
    cmd[0] = (id << 5);                             // ID + TEST UNIT READY

    memset(pDmaBuffer, 0, 512);
    ceCmd(ACSI_READ, cmd, CMD_LENGTH_SHORT, pDmaBuffer, 1);

    if(!hdIf.success || hdIf.statusByte != OK) {
        return FALSE;
    }

    // a device without the network module may answer as to a TEST UNIT READY - status OK, but no data. Check the data.
    if(strncmp((char *) pDmaBuffer, "CosmosEx network module", 23) != 0) {
        return FALSE;
    }

    deviceID        = id;
    requiredVersion = getWord (pDmaBuffer + 32);
    localIP         = getDword(pDmaBuffer + 34);
    netHandlesCount = getWord (pDmaBuffer + 38);        // since protocol 0x0101
    return TRUE;
}

static uint8_t scanIf(int ifType)
{
    uint8_t id;

    hdd_if_select(ifType);

    for(id = 0; id < 8; id++) {
        if(id == hdIf.scsiHostId) {                 // the host adapter itself
            continue;
        }

        if(netIdentify(id)) {
            return TRUE;
        }
    }

    return FALSE;
}

// the ID CE_DD uses - it publishes its hdIf in the CEPI cookie
static uint8_t tryCeDdDevice(void)
{
    THDif *ceHdIf = (THDif *) readCookie(0x43455049UL);    // 'CEPI'
    if(!ceHdIf) {
        return FALSE;
    }

    uint8_t id = ceHdIf->ceDeviceId;                // older CE_DD doesn't have it - whatever is there gets checked by IDENTIFY
    if(id > 7) {
        return FALSE;
    }

    int ifType;
    switch(ceHdIf->scsiHostId) {                    // which interface CE_DD uses
        case 0xff:  ifType = IF_ACSI;           break;
        case 7:     ifType = IF_SCSI_TT;        break;
        case 0:     ifType = IF_SCSI_FALCON;    break;
        default:    return FALSE;
    }

    hdd_if_select(ifType);
    return netIdentify(id);
}

uint8_t ceFindNetDevice(void)
{
    uint8_t found;

    hdIf.maxRetriesCount = 0;                       // devices on the other IDs won't answer - don't retry

    found = tryCeDdDevice();

    if(!found) {
        uint8_t machine = (uint8_t) (readCookie(0x5f4d4348UL) >> 16);   // '_MCH': 2 = TT, 3 = Falcon

        switch(machine) {
            case 2:  found = scanIf(IF_ACSI) || scanIf(IF_SCSI_TT); break;
            case 3:  found = scanIf(IF_SCSI_FALCON);                break;
            default: found = scanIf(IF_ACSI);                       break;
        }
    }

    hdIf.maxRetriesCount = 16;                      // as the old fake STinG
    return found;
}
