// Boot status - see boot_status.h and boot_status/status.s (the sector code, build it with boot_status/make_bootsector.py).
//
// While not connected to the host, core 1 answers the commands for the enabled IDs itself (core 0 may be blocked in the
// DHCP / connect for seconds):
// - READ(6) / READ(10) of the lowest enabled ID: sector 0 = the boot status sector, sector 1 = the status. Only reads
//   within sectors 0..1 - anything else, e.g. the 32 sectors CE_DD's boot sector loads from sector 1 when the connection
//   drops right after the chain to it, gets CHECK CONDITION, so nothing runs the status data as code.
// - REQUEST SENSE: NOT READY, logical unit is becoming ready.
// - HOSTMOD_RAW_NET (STinG / MiNTnet) commands: not handled here, core 0 answers them also without the host.
// - anything else: CHECK CONDITION.
// Once connected, all of it goes to the host as before.

#include "hardware/sync.h"

#include "defs.h"
#include "bridge.h"
#include "utils.h"
#include "command_handling.h"
#include "scsi.h"
#include "sting_net.h"
#include "boot_status.h"
#include "boot_status_data.h"

#define STATUS_TEXT_OFFSET  16

// written by core 0, read by core 1: the counter is changed last (and read first), so a change is always seen - at worst
// the text is a mix for a moment and gets printed again the next second
static uint8_t bsCode = BOOTST_STARTING;
static uint8_t bsOwnIp[4];
static uint8_t bsHostIp[4];
static volatile uint8_t bsCounter;

static uint8_t sector[512];     // core 1 only

static void changed(void)
{
    __dmb();
    bsCounter++;
}

void bootStatusSet(uint8_t code, const uint8_t* hostIp)
{
    uint8_t host[4] = {0, 0, 0, 0};
    if(hostIp) {
        memcpy(host, hostIp, 4);
    }

    if(code == bsCode && memcmp(host, bsHostIp, 4) == 0) {
        return;
    }

    bsCode = code;
    memcpy(bsHostIp, host, 4);
    changed();
}

void bootStatusSetOwnIp(const uint8_t* ip)
{
    if(memcmp(ip, bsOwnIp, 4) == 0) {
        return;
    }

    memcpy(bsOwnIp, ip, 4);
    changed();
}

//------------------
static void makeBootSector(uint8_t id)
{
    memcpy(sector, bootStatusSector, 512);
    sector[BOOT_STATUS_DEVNO_OFFSET] = IS_ACSI ? id : (8 + id);     // devno for DMAread()

    uint16_t sum = 0;                                   // executable: big endian words sum up to 0x1234
    for(int i = 0; i < 510; i += 2) {
        sum += (sector[i] << 8) | sector[i + 1];
    }
    sum = 0x1234 - sum;
    sector[510] = sum >> 8;
    sector[511] = sum;
}

static void makeStatusSector(uint8_t id)
{
    uint8_t counter = bsCounter;                        // first, see above
    __dmb();
    uint8_t code = bsCode;
    uint8_t own[4], host[4];
    memcpy(own, bsOwnIp, 4);
    memcpy(host, bsHostIp, 4);

    memset(sector, 0, 512);
    memcpy(sector, "CE2XSTAT", 8);
    sector[8] = code;
    sector[9] = counter;

    char state[160];
    switch(code) {
        case BOOTST_NO_ETHERNET:
            snprintf(state, sizeof(state), " Ethernet chip not found!\r\n The drive can't work - power it off and on.");
            break;

        case BOOTST_NO_LINK:
            snprintf(state, sizeof(state), " Ethernet cable not connected.");
            break;

        case BOOTST_WAIT_IP:
            snprintf(state, sizeof(state), " Waiting for an IP address (DHCP)...");
            break;

        case BOOTST_NO_DHCP:
            snprintf(state, sizeof(state), " No IP address from DHCP yet,\r\n still trying...");
            break;

        case BOOTST_SEARCHING:
            snprintf(state, sizeof(state), " IP address: %d.%d.%d.%d\r\n\r\n Looking for the Cosmo2ex host...",
                     own[0], own[1], own[2], own[3]);
            break;

        case BOOTST_CONNECTING:
            snprintf(state, sizeof(state), " IP address: %d.%d.%d.%d\r\n\r\n Connecting to the host:\r\n   %d.%d.%d.%d",
                     own[0], own[1], own[2], own[3], host[0], host[1], host[2], host[3]);
            break;

        default:
            snprintf(state, sizeof(state), " Starting...");
            break;
    }

    snprintf((char*) sector + STATUS_TEXT_OFFSET, 512 - STATUS_TEXT_OFFSET,
             "\033E\r\n Cosmo2ex hard drive - %s ID %d\r\n ------------------------------\r\n\r\n"
             "%s\r\n\r\n"
             " The boot goes on once the drive is\r\n"
             " connected to the host.\r\n"
             " (SPACE: continue without it)\r\n",
             IS_ACSI ? "ACSI" : "SCSI", id, state);
}

//------------------
static void sendStatus(uint8_t status)
{
    timeoutStart();
    onReadStatus(status);
    timeoutClear();
}

static void onRead(uint8_t id, uint32_t lba, uint32_t count)
{
    uint8_t bootId = __builtin_ctz(settings.enabledIDs);       // enabledIDs isn't 0 - the command came for one of them

    if(id != bootId || count == 0 || lba > 1 || count > (2 - lba)) {
        sendStatus(SCSI_ST_CHECK_CONDITION);
        return;
    }

    for(uint32_t i = 0; i < count; i++) {
        if(lba + i == 0) {
            makeBootSector(id);
        } else {
            makeStatusSector(id);
        }

        timeoutStart(CMD_TIMEOUT_ONESECOND);
        if(!onDataRead(512, sector)) {                  // ST gave up - no status, next command
            timeoutClear();
            return;
        }
    }

    sendStatus(SCSI_ST_OK);
}

static void onRequestSense(uint32_t len)
{
    memset(sector, 0, 512);
    sector[0]  = 0xf0;                                  // as the host: valid, current error
    sector[2]  = SCSI_E_NotReady;
    sector[7]  = (len > 7) ? (len - 7) : 0;
    sector[12] = SCSI_ASC_LU_NOT_READY;
    sector[13] = 0x01;                                  // becoming ready

    if(len > 0) {
        timeoutStart(CMD_TIMEOUT_ONESECOND);
        if(!onDataRead(len, sector)) {
            timeoutClear();
            return;
        }
    }

    sendStatus(SCSI_ST_OK);
}

bool bootStatusCommand(uint8_t* cmd, uint8_t cmdLen)
{
    bool icd = (cmd[0] & 0x1f) == 0x1f;
    uint8_t* p = icd ? cmd + 1 : cmd;
    uint8_t id = cmd[0] >> 5;

    if(cmdLen < (icd ? 7 : 6)) {
        sendStatus(SCSI_ST_CHECK_CONDITION);
        return true;
    }

    if((p[0] & 0x1f) == 0 && p[1] == 'C' && p[2] == 'E' && (p[3] & 0x7f) == HOSTMOD_RAW_NET) {    // network - core 0 does it
        return false;
    }

    if(!icd) {
        switch(p[0] & 0x1f) {
            case SCSI_C_READ6: {
                uint32_t lba = ((uint32_t) (p[1] & 0x1f) << 16) | ((uint32_t) p[2] << 8) | p[3];
                onRead(id, lba, p[4] ? p[4] : 256);
                return true;
            }

            case SCSI_C_REQUEST_SENSE:
                onRequestSense(p[4]);
                return true;
        }
    } else if(p[0] == SCSI_C_READ10 && cmdLen >= 11) {
        onRead(id, getDword(p + 2), getWord(p + 7));
        return true;
    }

    sendStatus(SCSI_ST_CHECK_CONDITION);                // not ready yet
    return true;
}
