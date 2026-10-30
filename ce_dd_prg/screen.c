// vim: expandtab shiftwidth=4 tabstop=4
#include <mint/osbind.h>
#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/hdd_if.h"
#include "../libacsiscsi/mutex.h"
#include "../libacsiscsi/stdlib.h"
#include "stdlib.h"
#include "translated.h"

#include "ce_dd_prg.h"
#include "main.h"
#include "screen.h"
#include "screencast.h"

// Screencast and screenshots from VBL. The VBL is always installed: every SCREENCAST_POLL_VBLS it asks the host
// whether to take a screenshot, whether screencast is on (web UI is retrieving frames) and how often to send frames.
// Only when one of those is on it sends screens, so with screencast off it costs one short command every 3 seconds.
// To disable all of this (if it makes trouble), comment out Supexec(init_screencapture) in main() in main.c.

#define SCREENCAST_POLL_VBLS    150         // ask host for screencast / screenshot config every 3 s (at 50 Hz)

extern volatile mutex mtx;
extern uint8_t deviceID;

static uint16_t pollCount;                  // VBLs since the last config poll
static uint16_t frameCount;                 // VBLs since the last sent screencast frame
static uint8_t  castActive;                 // host wants screencast frames
static uint8_t  castFrameSkip = 10;         // send screencast frame every this many VBLs
static uint8_t  takeScreenShot;             // host wants one screenshot

// own command bytes and buffer - the VBL can come while CE_DD code has filled the global ones and is about to send them
static uint8_t  vblCmdShort[CMD_LENGTH_SHORT] = {         0, 'C', 'E', HOSTMOD_TRANSLATED_DISK, 0, 0};
static uint8_t  vblCmdLong [CMD_LENGTH_LONG]  = {0x1f, 0xA0, 'C', 'E', HOSTMOD_TRANSLATED_DISK, 0, 0, 0, 0, 0, 0, 0, 0};
static uint16_t vblBuffer[512 / 2];         // uint16_t to have it word aligned for DMA; palette is up to 512 bytes

static void pollConfig(void);
static void sendScreen(void);
static uint8_t writeScreen(uint8_t command, uint8_t screenmode, uint8_t *bfr, uint32_t offset, uint32_t cnt);

static uint32_t getdma(void)
{
    return (*dmaAddrLo) | ((*dmaAddrMid) << 8) | ((*dmaAddrHi) << 16);
}

// called on every VBL (without FLOCK) - returns non-zero when screenworker() has something to do
uint8_t screenVblTick(void)
{
    pollCount++;
    frameCount++;

    if(pollCount >= SCREENCAST_POLL_VBLS) {                 // time to ask host?
        return 1;
    }

    if(takeScreenShot || (castActive && frameCount >= castFrameSkip)) {    // time to send screen?
        return 1;
    }

    return 0;
}

// called from VBL with FLOCK acquired, when screenVblTick() said there's work
void screenworker(void)
{
    if(mutex_trylock(&mtx) == 0) {          // CE_DD is talking to host now? try again on next VBL (counters keep going)
        return;
    }

    // the interrupted code might be just about to read results of its command, so keep all of it as it was
    THDif savedHdIf = hdIf;
    uint32_t dmaAddr = getdma();

    hdIf.forceFlock      = 1;               // FLOCK has been acquired in the asm code before this function
    hdIf.maxRetriesCount = 0;               // no retries - don't block VBL for long; next one will come soon anyway

    if(pollCount >= SCREENCAST_POLL_VBLS) {
        pollCount = 0;
        pollConfig();
    }

    if(takeScreenShot || (castActive && frameCount >= castFrameSkip)) {
        frameCount      = 0;
        takeScreenShot  = 0;                // one screenshot only
        sendScreen();
    }

    setdma(dmaAddr);
    hdIf = savedHdIf;
    mutex_unlock(&mtx);
}

static void pollConfig(void)
{
    uint8_t *bfr = (uint8_t *) vblBuffer;

    vblCmdShort[0] = (deviceID << 5);
    vblCmdShort[4] = TRAN_CMD_SCREENSHOT_CONFIG;

    (*hdIf.cmd_nolock)(ACSI_READ, vblCmdShort, CMD_LENGTH_SHORT, bfr, 1);

    if(!hdIf.success || hdIf.statusByte != OK) {    // error? keep the previous config
        return;
    }

    // byte 0 (screenshots VBL enabled) is not used anymore - the VBL is always installed
    if(bfr[1]) {                            // 1: take screenshot?
        takeScreenShot = 1;
    }

    castActive = bfr[2];                    // 2: screencast on?

    if(bfr[3] >= 2) {                       // 3: VBLs between screencast frames (0 from older host - keep the default)
        castFrameSkip = bfr[3];
    }
}

static void sendScreen(void)
{
    uint8_t *pxScreen   = screencastScreenAddress();
    uint8_t  res        = screencastGetRes();           // same resolution for screen and palette
    uint32_t screenSize = screencastScreenSize(res);
    uint32_t offset;

    // send screen memory - in parts, as one DMA transfer can't do more than MAXSECTORS
    for(offset = 0; offset < screenSize; offset += SCREENCAST_PART_SIZE) {
        uint32_t partSize = screenSize - offset;
        if(partSize > SCREENCAST_PART_SIZE) {
            partSize = SCREENCAST_PART_SIZE;
        }

        if(!writeScreen(TRAN_CMD_SENDSCREENCAST, res, pxScreen + offset, offset, partSize)) {
            return;                         // failed? don't send the rest of it
        }
    }

    // send palette - last and always, the host takes the resolution of the frame from it
    uint8_t *bfr = (uint8_t *) vblBuffer;
    uint32_t paletteSize = screencastGetPalette(res, bfr);
    writeScreen(TRAN_CMD_SCREENCASTPALETTE, res, bfr, 0, paletteSize);
}

static uint8_t writeScreen(uint8_t command, uint8_t screenmode, uint8_t *bfr, uint32_t offset, uint32_t cnt)
{
    vblCmdLong[0] = (deviceID << 5) | 0x1f; // cmd[0] = ACSI_id + ICD command marker (0x1f)
    vblCmdLong[5] = command;
    vblCmdLong[6] = screenmode;

    vblCmdLong[7] = cnt >> 16;              // store byte count
    vblCmdLong[8] = cnt >>  8;
    vblCmdLong[9] = cnt  & 0xff;

    vblCmdLong[10] = offset >> 16;          // where this part goes in the screen
    vblCmdLong[11] = offset >>  8;
    vblCmdLong[12] = offset  & 0xff;

    uint16_t sectorCount = (cnt + 511) >> 9;    // calculate how many sectors should we transfer

    (*hdIf.cmd_nolock)(ACSI_WRITE, vblCmdLong, CMD_LENGTH_LONG, bfr, sectorCount);    // send command to host

    return hdIf.success;
}
