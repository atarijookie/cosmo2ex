#include <stdio.h>
#include <string.h>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../acsidatatrans.h"
#include "../acsicommand/screencastacsicommand.h"
#include "browser_commands.h"
#include "browsermodule.h"
#include "browserhistory.h"

BrowserModule::BrowserModule(AcsiDataTrans *dt)
{
    cmd       = NULL;
    dataTrans = dt;
}

BrowserModule::~BrowserModule()
{
}

void BrowserModule::reset(void)
{
    link.stop();                // stops ce_browser too - the next IDENTIFY starts it again
    mode = BrowserMode();
    sending.reset();
}

bool BrowserModule::resolutionInfo(uint8_t res, uint16_t *width, uint16_t *height, uint8_t *planes)
{
    switch(res) {
        case SCREENCAST_RES_ST_LOW:     *width = 320;  *height = 200; *planes = 4; return true;
        case SCREENCAST_RES_ST_MID:     *width = 640;  *height = 200; *planes = 2; return true;
        case SCREENCAST_RES_ST_HIGH:    *width = 640;  *height = 400; *planes = 1; return true;
        case SCREENCAST_RES_TT_LOW:     *width = 320;  *height = 480; *planes = 8; return true;
        case SCREENCAST_RES_TT_MID:     *width = 640;  *height = 480; *planes = 4; return true;
        case SCREENCAST_RES_TT_HIGH:    *width = 1280; *height = 960; *planes = 1; return true;
    }
    return false;
}

void BrowserModule::processCommand(uint8_t *command)
{
    cmd = command;

    if(!dataTrans) {
        logHdd(LOG_ERROR, "BrowserModule::processCommand - no dataTrans");
        return;
    }

    dataTrans->clear();
    logHdd(LOG_DEBUG, "BrowserModule::processCommand - %s", cmdName(cmd[4]));
    link.noteActivity();

    switch(cmd[4]) {
        case BRW_CMD_IDENTIFY:      identify();     break;
        case BRW_CMD_SET_MODE:      setMode();      break;
        case BRW_CMD_NAVIGATE:      navigate();     break;
        case BRW_CMD_NAV_ACTION:    navAction();    break;
        case BRW_CMD_MOUSE:         mouse();        break;
        case BRW_CMD_KEY:           key();          break;
        case BRW_CMD_SCROLL:        scroll();       break;
        case BRW_CMD_GET_UPDATE:    getUpdate();    break;
        case BRW_CMD_GET_STATUS:    getStatus();    break;
        case BRW_CMD_GET_HISTORY:   getHistory();   break;
        default:
            logHdd(LOG_WARNING, "BrowserModule - unknown command 0x%02x", cmd[4]);
            dataTrans->setStatus(BRW_ERR_BAD_PARAM);
            break;
    }

    dataTrans->sendDataAndStatus();
}

void BrowserModule::linkResult(bool sent)
{
    dataTrans->setStatus(sent ? BRW_OK : BRW_ERR_NOT_RUNNING);
}

//--------------------------------------------------------

void BrowserModule::identify(void)
{
    bool listening = link.start();      // CE_BROWS.PRG just started - make sure ce_browser runs

    dataTrans->addDataBfr("CosmosEx browser module", 24, false);  // 23 chars + terminator
    dataTrans->addZerosUntilSize(32);
    dataTrans->addDataWord(BROWSER_MODULE_VERSION);             // 32
    dataTrans->addDataByte(link.isConnected() ? BRW_IDF_CONNECTED : 0);    // 34
    dataTrans->addZerosUntilSize(48);

    if(!listening) {
        logHdd(LOG_ERROR, "BrowserModule::identify - can't listen for ce_browser");
    }
    dataTrans->setStatus(BRW_OK);
}

// [5] res, [6..7] page area height, [8] zoom in quarters. Kept even when ce_browser isn't connected - it gets it when it connects.
void BrowserModule::setMode(void)
{
    BrowserMode m;
    uint16_t screenH;

    m.res = cmd[5];
    if(!resolutionInfo(m.res, &m.width, &screenH, &m.planes)) {
        logHdd(LOG_WARNING, "BrowserModule::setMode - unsupported resolution 0x%02x", m.res);
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }

    m.height = Utils::getWord(cmd + 6);
    m.zoom   = cmd[8] ? cmd[8] : 4;
    if(m.height < 16 || m.height > screenH || m.zoom < 2 || m.zoom > 16) {
        logHdd(LOG_WARNING, "BrowserModule::setMode - bad page height %d or zoom %d", (int) m.height, (int) m.zoom);
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }

    m.valid = true;
    mode = m;
    sending.reset();
    link.setMode(m);

    logHdd(LOG_INFO, "BrowserModule::setMode - res 0x%02x, page %dx%d, %d planes, zoom %d/4",
           (int) m.res, (int) m.width, (int) m.height, (int) m.planes, (int) m.zoom);
    dataTrans->setStatus(BRW_OK);
}

// data: 512 B, zero terminated URL. Kept even when ce_browser isn't connected - it gets it when it connects.
void BrowserModule::navigate(void)
{
    uint8_t buf[BRW_URL_SIZE];
    if(!dataTrans->recvData(buf, sizeof(buf))) {
        dataTrans->setStatus(BRW_ERR_INTERNAL);
        return;
    }
    buf[sizeof(buf) - 1] = 0;

    std::string url((const char *) buf);
    if(url.empty()) {
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }

    logHdd(LOG_INFO, "BrowserModule::navigate - %s", url.c_str());
    link.navigate(url);
    dataTrans->setStatus(BRW_OK);
}

void BrowserModule::navAction(void)
{
    if(cmd[5] > BRW_NAV_LAST) {
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }
    linkResult(link.navAction(cmd[5]));
}

void BrowserModule::mouse(void)
{
    if(cmd[5] > BRW_MOUSE_LAST) {
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }
    linkResult(link.mouse(cmd[5], Utils::getWord(cmd + 6), Utils::getWord(cmd + 8), cmd[10]));
}

void BrowserModule::key(void)
{
    linkResult(link.key(cmd[5], cmd[6], cmd[7]));
}

void BrowserModule::scroll(void)
{
    linkResult(link.scroll((int8_t) cmd[5], (int8_t) cmd[6], (int8_t) cmd[7], (int8_t) cmd[8]));
}

// [5..8] seq the ST has completely, [9] max sectors, [10..11] row to resume from - reply format in browser_commands.h
void BrowserModule::getUpdate(void)
{
    uint32_t baseSeq    = Utils::getDword(cmd + 5);
    int      maxSectors = cmd[9];
    int      resumeRow  = Utils::getWord(cmd + 10);

    if(!mode.valid) {
        dataTrans->setStatus(BRW_ERR_NO_MODE);
        return;
    }
    if(!link.isConnected()) {
        dataTrans->setStatus(BRW_ERR_NOT_RUNNING);
        return;
    }

    maxSectors = MAX(1, MIN(maxSectors, ACSI_MAX_TRANSFER_SIZE_BYTES / 512));

    std::shared_ptr<const BrowserFrame> f;
    int startRow = 0;

    if(resumeRow > 0 && sending && resumeRow < sending->height) {      // next part of the frame being sent
        f = sending;
        startRow = resumeRow;
    } else {                                                           // new update (or the ST asks for a part we don't have - start over)
        f = link.latestFrame();
        if(!f || f->seq == baseSeq) {
            dataTrans->addDataDword(baseSeq);
            dataTrans->addDataByte(BRW_UPD_NO_CHANGE);
            dataTrans->addDataByte(mode.res);
            dataTrans->addZerosUntilSize(BRW_UPD_HEADER_SIZE);
            dataTrans->setStatus(BRW_OK);
            return;
        }
    }

    uint32_t palCount  = (startRow == 0) ? f->palette.size() : 0;      // palette with the 1st part only
    uint32_t rowBytes  = f->rowBytes();
    int32_t  avail     = maxSectors * 512 - BRW_UPD_HEADER_SIZE - palCount * 2 - BRW_UPD_RECT_HEADER_SIZE;
    int      rows      = (avail > 0) ? (int) (avail / rowBytes) : 0;
    rows = MIN(rows, f->height - startRow);

    if(rows <= 0) {
        logHdd(LOG_WARNING, "BrowserModule::getUpdate - %d sectors is too little for one row", maxSectors);
        dataTrans->setStatus(BRW_ERR_BAD_PARAM);
        return;
    }

    bool more = (startRow + rows) < f->height;
    if(more) {
        sending = f;
    } else {
        sending.reset();
    }

    uint8_t flags = BRW_UPD_FULL | (palCount ? BRW_UPD_PALETTE : 0) | (more ? BRW_UPD_MORE : 0);

    dataTrans->addDataDword(f->seq);            // 0
    dataTrans->addDataByte(flags);              // 4
    dataTrans->addDataByte(f->res);             // 5
    dataTrans->addDataWord(1);                  // 6: rect count
    dataTrans->addDataWord(palCount);           // 8
    dataTrans->addDataWord(f->width);           // 10
    dataTrans->addDataWord(f->height);          // 12
    dataTrans->addDataByte(f->planes);          // 14
    dataTrans->addDataByte(f->zoom);            // 15

    for(uint32_t i=0; i<palCount; i++) {
        dataTrans->addDataWord(f->palette[i]);
    }

    dataTrans->addDataWord(0);                  // rect x / 16
    dataTrans->addDataWord(startRow);           // rect y
    dataTrans->addDataWord(f->width / 16);      // rect width / 16
    dataTrans->addDataWord(rows);               // rect height
    dataTrans->addDataBfr(f->data.data() + (size_t) startRow * rowBytes, (uint32_t) rows * rowBytes, true);

    logHdd(LOG_DEBUG, "BrowserModule::getUpdate - frame %u rows %d..%d of %d%s", f->seq, startRow, startRow + rows - 1,
           (int) f->height, more ? ", more follows" : "");
    dataTrans->setStatus(BRW_OK);
}

static void putString(uint8_t *buf, int size, const std::string& s)
{
    int len = MIN((int) s.size(), size - 1);
    memcpy(buf, s.data(), len);
    buf[len] = 0;
}

void BrowserModule::getStatus(void)
{
    BrowserStatus s = link.status();

    uint8_t buf[BRW_STATUS_SIZE];
    memset(buf, 0, sizeof(buf));

    buf[0] = s.flags | (s.connected ? BRW_STF_CONNECTED : 0);
    buf[1] = s.progress;
    Utils::storeWord (buf + 2, s.caretX);
    Utils::storeWord (buf + 4, s.caretY);
    Utils::storeDword(buf + 8, s.latestSeq);
    putString(buf + BRW_STATUS_URL_OFFSET,   BRW_STATUS_URL_SIZE,   s.url);
    putString(buf + BRW_STATUS_TITLE_OFFSET, BRW_STATUS_TITLE_SIZE, s.title);
    putString(buf + BRW_STATUS_MSG_OFFSET,   BRW_STATUS_MSG_SIZE,   s.message);

    dataTrans->addDataBfr(buf, sizeof(buf), false);
    dataTrans->setStatus(BRW_OK);
}

// [5] max URLs (0 = all), [6] max sectors. Reply: count (W), URLs zero terminated, newest first - as many as fit.
// Works without ce_browser - the history is kept by ce_cores.
void BrowserModule::getHistory(void)
{
    uint32_t maxBytes = MAX(1, MIN((int) cmd[6], ACSI_MAX_TRANSFER_SIZE_BYTES / 512)) * 512;
    std::vector<std::string> urls = BrowserHistory::list(cmd[5]);

    std::vector<uint8_t> out(2, 0);
    uint16_t count = 0;
    for(const std::string& u : urls) {
        if(out.size() + u.size() + 1 > maxBytes) {
            break;
        }
        out.insert(out.end(), u.begin(), u.end());
        out.push_back(0);
        count++;
    }
    Utils::storeWord(out.data(), count);

    dataTrans->addDataBfr(out.data(), out.size(), true);
    dataTrans->setStatus(BRW_OK);
}

const char *BrowserModule::cmdName(uint8_t c)
{
    switch(c) {
        case BRW_CMD_IDENTIFY:      return "IDENTIFY";
        case BRW_CMD_SET_MODE:      return "SET_MODE";
        case BRW_CMD_NAVIGATE:      return "NAVIGATE";
        case BRW_CMD_NAV_ACTION:    return "NAV_ACTION";
        case BRW_CMD_MOUSE:         return "MOUSE";
        case BRW_CMD_KEY:           return "KEY";
        case BRW_CMD_SCROLL:        return "SCROLL";
        case BRW_CMD_GET_UPDATE:    return "GET_UPDATE";
        case BRW_CMD_GET_STATUS:    return "GET_STATUS";
        case BRW_CMD_GET_HISTORY:   return "GET_HISTORY";
    }
    return "unknown";
}
