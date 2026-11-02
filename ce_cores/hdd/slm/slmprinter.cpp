#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "../../misc/global.h"
#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../../misc/settings.h"
#include "../../chipinterface/chipinterfacedefs.h"
#include "../acsidatatrans.h"
#include "slmprinter.h"
#include "stb_image_write.h"

// absolute (maximum) values of parameters - legal paper is the biggest
#define ABS_BLOCK_HEIGHT    4080
#define ABS_BLOCK_WIDTH     2400
#define ABS_TOP_MARGIN      300
#define ABS_LEFT_MARGIN     150
#define ABS_PAPER_FLAGS     0x01        // only manual feed is supported
#define ABS_SYSTEM_TIMEOUT  255
#define RESOLUTION_DPI      300

static const SlmPaper papers[] = {
    //  key       width height left top
    { "letter",   2400, 3180,  75,  60 },       // 8.5 x 11 in
    { "legal",    2400, 4080,  75,  60 },       // 8.5 x 14 in
    { "a4",       2336, 3386,  72,  61 },       // 210 x 297 mm
    { "b5",       2016, 2914,  67,  60 },       // 182 x 257 mm
};

SlmPrinter::SlmPrinter(AcsiDataTrans* dataTrans)
{
    this->dataTrans = dataTrans;
    paper = paperByKey(SLM_PAPER_DEFAULT);
    macHex = "000000000000";
    reset();
}

void SlmPrinter::setMac(const uint8_t* mac)
{
    char hex[16];
    Settings::binToHex((uint8_t*) mac, 6, hex);
    macHex = hex;
}

void SlmPrinter::setWaitForCommand(WaitForCommandFn fn)
{
    waitForCommand = fn;
}

void SlmPrinter::reset(void)
{
    params = defaultParams(paper);
    pageCount = 0;
}

const SlmPaper* SlmPrinter::paperByKey(const std::string& key)
{
    for(const SlmPaper& p : papers) {
        if(key == p.key) {
            return &p;
        }
    }

    return (key == SLM_PAPER_DEFAULT) ? &papers[0] : paperByKey(SLM_PAPER_DEFAULT);
}

// Paper size is a global setting (Settings page of web UI). Like on the real printer, when another paper is put in the
// cassette, block size and margins change to fit it - even after MODE SELECT.
void SlmPrinter::updatePaper(void)
{
    Settings s;
    const SlmPaper* newPaper = paperByKey(s.getString(SLM_PAPER_SETTING, SLM_PAPER_DEFAULT));

    if(newPaper == paper) {
        return;
    }

    logHdd(LOG_INFO, "SlmPrinter::updatePaper - paper changed from %s to %s", paper->key, newPaper->key);
    paper = newPaper;

    params.blockWidth   = paper->blockWidth;
    params.blockHeight  = paper->blockHeight;
    params.leftMargin   = paper->leftMargin;
    params.topMargin    = paper->topMargin;
}

SlmParams SlmPrinter::defaultParams(const SlmPaper* paper)
{
    SlmParams p;
    p.blockHeight   = paper->blockHeight;
    p.blockWidth    = paper->blockWidth;
    p.topMargin     = paper->topMargin;
    p.leftMargin    = paper->leftMargin;
    p.paperFlags    = 0;
    p.systemTimeout = 30;
    p.outputFlags   = 0;
    return p;
}

bool SlmPrinter::processCommand(const uint8_t* cmdIn, uint8_t* nextCmd)
{
    uint8_t cmd[6];
    memcpy(cmd, cmdIn, 6);                  // nextCmd might be the same buffer as cmdIn

    uint8_t opcode = cmd[0] & 0x1f;
    uint8_t device = cmd[1] >> 5;
    uint8_t statusBase = device << 5;

    logHdd(LOG_DEBUG, "SlmPrinter::processCommand - %02x %02x %02x %02x %02x %02x", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);

    updatePaper();                          // paper size might have been changed in web UI

    if(opcode == SLM_OP_STOP_PRINT) {       // STOP PRINT outside of multi-page PRINT - drivers don't read status after it, so don't send any
        logHdd(LOG_DEBUG, "SlmPrinter::processCommand - STOP PRINT while not printing, ignored");
        endCommandWithoutStatus();
        return false;
    }

    if(device != 0) {                       // there's just one printer on the controller
        sendStatus(statusBase | SLM_ST_INVALID_DEVICE);
        return false;
    }

    switch(opcode) {
        case SLM_OP_REQUEST_SENSE:  sendStatus(statusBase | SLM_ST_OK);         break;     // emulated printer is always ready
        case SLM_OP_INQUIRY:        onInquiry(cmd, statusBase);                 break;
        case SLM_OP_MODE_SELECT:    onModeSelect(cmd, statusBase);              break;
        case SLM_OP_MODE_SENSE:     onModeSense(cmd, statusBase);               break;
        case SLM_OP_PRINT:          return onPrint(cmd, statusBase, nextCmd);
        default:                    sendStatus(statusBase | SLM_ST_INVALID_OPCODE); break;
    }

    return false;
}

void SlmPrinter::sendStatus(uint8_t statusByte)
{
    dataTrans->sendStatusToHans(statusByte);
}

// command without status (STOP PRINT) - the device just has to go back to waiting for the next command
void SlmPrinter::endCommandWithoutStatus(void)
{
    dataTrans->pioRead(NULL, 0);
}

//---------------------------------------
// INQUIRY

int SlmPrinter::buildInquiry(uint8_t* out) const
{
    int len = strlen(SLM_ID_STRING);

    out[0] = 0x02;              // device type: printer
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t) len;     // string length
    memcpy(out + 5, SLM_ID_STRING, len);

    return 5 + len;
}

void SlmPrinter::onInquiry(const uint8_t* cmd, uint8_t statusBase)
{
    // without the Extended Status bit the data would have to go by DMA, which the controller can't do. Drivers send INQUIRY
    // without this bit first and set it only when this fails.
    if((cmd[5] & 0x80) == 0) {
        sendStatus(statusBase | SLM_ST_INVALID_OPCODE);
        return;
    }

    uint8_t bfr[PIO_READ_MAX_LEN];
    bfr[0] = statusBase | SLM_ST_OK;            // null status byte before the identification list
    int len = 1 + buildInquiry(bfr + 1);

    dataTrans->pioRead(bfr, len);
}

//---------------------------------------
// MODE SENSE / MODE SELECT

int SlmPrinter::buildParamList(uint8_t* out, bool absolute) const
{
    SlmParams p = params;

    if(absolute) {
        p.blockHeight   = ABS_BLOCK_HEIGHT;
        p.blockWidth    = ABS_BLOCK_WIDTH;
        p.topMargin     = ABS_TOP_MARGIN;
        p.leftMargin    = ABS_LEFT_MARGIN;
        p.paperFlags    = ABS_PAPER_FLAGS;
        p.systemTimeout = ABS_SYSTEM_TIMEOUT;
        p.outputFlags   = 0;
    }

    out[0] = SLM_PARAM_LIST_LEN;
    Utils::storeWord(out +  1, p.blockHeight);
    Utils::storeWord(out +  3, p.blockWidth);
    Utils::storeWord(out +  5, p.topMargin);
    Utils::storeWord(out +  7, p.leftMargin);
    out[9] = p.paperFlags;
    Utils::storeWord(out + 10, RESOLUTION_DPI);         // vertical resolution
    Utils::storeWord(out + 12, RESOLUTION_DPI);         // horizontal resolution
    out[14] = p.systemTimeout;
    Utils::storeWord(out + 15, SLM_SCAN_TIME_US);
    Utils::storeWord(out + 17, pageCount);
    Utils::storeWord(out + 19, SLM_INPUT_CAPACITY);
    Utils::storeWord(out + 21, SLM_OUTPUT_CAPACITY);
    out[23] = p.outputFlags;

    return 1 + SLM_PARAM_LIST_LEN;
}

void SlmPrinter::onModeSense(const uint8_t* cmd, uint8_t statusBase)
{
    bool absolute = (cmd[5] & 0x80) != 0;
    int  wanted   = cmd[4];                                 // 0 = whole list

    uint8_t list[1 + SLM_PARAM_LIST_LEN];
    buildParamList(list, absolute);

    int count = (wanted == 0 || wanted > SLM_PARAM_LIST_LEN) ? SLM_PARAM_LIST_LEN : wanted;
    list[0] = count;                                        // list length = count of bytes really returned

    uint8_t bfr[2 + SLM_PARAM_LIST_LEN];
    bfr[0] = statusBase | SLM_ST_OK;                        // status byte before the list
    memcpy(bfr + 1, list, 1 + count);

    dataTrans->pioRead(bfr, 2 + count);
}

// list - parameter list without the list length byte (list[0] is byte 1 of MODE SENSE list)
uint8_t SlmPrinter::applyModeSelect(const uint8_t* list, int len)
{
    uint8_t cur[1 + SLM_PARAM_LIST_LEN];
    buildParamList(cur, false);                             // start with current values, the host might send just part of the list

    int cnt = (len > SLM_PARAM_LIST_LEN) ? SLM_PARAM_LIST_LEN : len;
    memcpy(cur + 1, list, cnt);

    SlmParams p = params;
    p.blockHeight   = Utils::getWord(cur + 1);
    p.blockWidth    = Utils::getWord(cur + 3);
    p.topMargin     = Utils::getWord(cur + 5);
    p.leftMargin    = Utils::getWord(cur + 7);
    p.paperFlags    = cur[9];
    p.systemTimeout = cur[14];
    p.outputFlags   = cur[23];          // resolutions, scan time, page count and capacities are read only

    bool bad = p.blockHeight == 0 || p.blockHeight > ABS_BLOCK_HEIGHT ||
               p.blockWidth  == 0 || p.blockWidth  > ABS_BLOCK_WIDTH  ||
               p.topMargin  > ABS_TOP_MARGIN || p.leftMargin > ABS_LEFT_MARGIN ||
               (p.paperFlags & ~ABS_PAPER_FLAGS) != 0 || p.outputFlags != 0;

    if(bad) {
        logHdd(LOG_WARNING, "SlmPrinter::applyModeSelect - invalid parameters: height %d, width %d, top %d, left %d, flags %02x, output %02x",
               p.blockHeight, p.blockWidth, p.topMargin, p.leftMargin, p.paperFlags, p.outputFlags);
        return SLM_ST_INVALID_PARAMS;
    }

    params = p;
    logHdd(LOG_DEBUG, "SlmPrinter::applyModeSelect - height %d, width %d, top %d, left %d, flags %02x, timeout %d",
           p.blockHeight, p.blockWidth, p.topMargin, p.leftMargin, p.paperFlags, p.systemTimeout);
    return SLM_ST_OK;
}

void SlmPrinter::onModeSelect(const uint8_t* cmd, uint8_t statusBase)
{
    if(cmd[5] & 0x80) {                         // Reset Default - no list is sent
        params = defaultParams(paper);
        sendStatus(statusBase | SLM_ST_OK);
        return;
    }

    uint8_t listLen = 0;
    if(!dataTrans->pioWrite(&listLen, 1)) {     // get list length (device returns to waiting for command on failure)
        logHdd(LOG_WARNING, "SlmPrinter::onModeSelect - failed to get list length");
        return;
    }

    uint8_t list[256];
    if(listLen > 0 && !dataTrans->pioWrite(list, listLen)) {
        logHdd(LOG_WARNING, "SlmPrinter::onModeSelect - failed to get %d bytes of list", listLen);
        return;
    }

    sendStatus(statusBase | applyModeSelect(list, listLen));
}

//---------------------------------------
// PRINT

bool SlmPrinter::onPrint(const uint8_t* cmd, uint8_t statusBase, uint8_t* nextCmd)
{
    uint8_t transferLength = cmd[4];
    bool indefinite = (transferLength == SLM_PRINT_INDEFINITE);
    int  pagesTotal = (transferLength == 0) ? 1 : transferLength;

    if(cmd[5] & 0x40) {
        logHdd(LOG_WARNING, "SlmPrinter::onPrint - Generate H-Sync not supported, printing without it");
    }

    uint32_t bpl   = bytesPerLine();
    uint32_t total = pageBytes();
    uint16_t firstBurst = (uint16_t) ((bpl + 2 < total) ? (bpl + 2) : total);    // + two-byte FIFO of the controller

    std::vector<uint8_t> page(total);
    time_t jobTime = time(NULL);

    logHdd(LOG_INFO, "SlmPrinter::onPrint - transfer length %d, page %d x %d, %d bytes", transferLength, params.blockWidth, params.blockHeight, total);

    for(int pageNo = 1; ; pageNo++) {
        bool good = dataTrans->recvDataPaced(page.data(), total, firstBurst, (uint16_t) bpl, SLM_SCAN_TIME_US,
                                             SLM_START_DELAY_MS, statusBase | SLM_ST_SYSTEM_TIMEOUT);

        if(!good) {     // ST didn't send any data - the device sent SYSTEM TIMEOUT status by itself (ST stopping mid-page is fine, rest is blank)
            logHdd(LOG_WARNING, "SlmPrinter::onPrint - page %d not received, print aborted", pageNo);
            return false;
        }

        pageCount++;
        savePageAsync(page.data(), jobTime, pageNo);
        sendStatus(statusBase | SLM_ST_OK);

        if(!indefinite && pageNo >= pagesTotal) {       // last page done
            return false;
        }

        // more pages to go - in this window the host can send STOP PRINT, which ends printing after the next page
        if(!waitForCommand) {
            continue;
        }

        uint8_t next[ACSI_CMD_SIZE];
        if(!waitForCommand(SLM_STOP_PRINT_WINDOW_MS, next)) {   // nothing came, next page
            continue;
        }

        if((next[0] >> 5) == (cmd[0] >> 5) && (next[0] & 0x1f) == SLM_OP_STOP_PRINT) {
            logHdd(LOG_DEBUG, "SlmPrinter::onPrint - STOP PRINT, page %d will be the last", pageNo + 1);
            endCommandWithoutStatus();
            indefinite = false;
            pagesTotal = pageNo + 1;
            continue;
        }

        logHdd(LOG_WARNING, "SlmPrinter::onPrint - got other command %02x %02x while printing, print aborted", next[0], next[1]);
        memcpy(nextCmd, next, ACSI_CMD_SIZE);
        return true;
    }
}

//---------------------------------------
// page output

void SlmPrinter::bitmapToGray(const uint8_t* bitmap, uint32_t bytesPerLine, uint32_t width, uint32_t height, uint8_t* gray)
{
    for(uint32_t y = 0; y < height; y++) {
        const uint8_t* src = bitmap + y * bytesPerLine;
        uint8_t* dst = gray + y * width;

        for(uint32_t x = 0; x < width; x++) {
            bool mark = (src[x >> 3] & (0x80 >> (x & 7))) != 0;     // 1 = mark
            dst[x] = mark ? 0x00 : 0xff;
        }
    }
}

std::string SlmPrinter::pageFileName(const std::string& macHex, time_t jobTime, int pageNo)
{
    struct tm tm;
    localtime_r(&jobTime, &tm);

    char name[64];
    snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d_p%03d.png", macHex.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, pageNo);
    return name;
}

// PNG encoding takes a while - don't let the next page wait for it
void SlmPrinter::savePageAsync(const uint8_t* bitmap, time_t jobTime, int pageNo)
{
    uint32_t bpl    = bytesPerLine();
    uint32_t width  = params.blockWidth;
    uint32_t height = params.blockHeight;

    std::string dir = Utils::dotEnvValue("PRINTOUTS_DIR", "./printouts", false);
    std::string path = Utils::mergeHostPaths2(dir, pageFileName(macHex, jobTime, pageNo));

    std::vector<uint8_t> copy(bitmap, bitmap + bpl * height);

    std::thread([copy, bpl, width, height, dir, path]() {
        Utils::mkpath(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

        std::vector<uint8_t> gray((size_t) width * height);
        bitmapToGray(copy.data(), bpl, width, height, gray.data());

        if(stbi_write_png(path.c_str(), width, height, 1, gray.data(), width)) {
            logHdd(LOG_INFO, "SlmPrinter - page saved to %s", path.c_str());
        } else {
            logHdd(LOG_ERROR, "SlmPrinter - failed to save page to %s", path.c_str());
        }
    }).detach();
}
