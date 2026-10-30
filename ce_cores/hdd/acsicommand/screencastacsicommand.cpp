#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

#include "screencastacsicommand.h"
#include "../../misc/global.h"
#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../translated/gemdos_errno.h"

ScreencastAcsiCommand::ScreencastAcsiCommand(AcsiDataTrans *dt):dataTrans(dt)
{
    dataBuffer  = new uint8_t[SCREENCAST_BUFFER_SIZE];
}

ScreencastAcsiCommand::~ScreencastAcsiCommand()
{
	delete []dataBuffer;
}

// latest frame - written by HDD thread (readScreen, readPalette), read by command socket thread (frameGet)
static uint8_t frame[SCREENCAST_FRAME_SIZE_MAX];
static uint32_t frameSeq;                           // incremented with each whole frame from ST (on its palette, which comes last)
static pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;

void ScreencastAcsiCommand::frameInit(void)
{
    pthread_mutex_lock(&frameMutex);
    memset(frame, 0, sizeof(frame));                // set all to zeros - resolution ST low

    std::string logoPath = Utils::dotEnvValue("CE_LOGO_FILE", "ce_logo.bin");   // path to logo file
    FILE *f = fopen(logoPath.c_str(), "rb");        // if can open this logo file
    if(f) {                                         // fill the initial frame with logo - ST palette (32 B) and ST screen data
        size_t got = fread(frame + 1, 1, 32, f);
        if(got == 32) {
            fread(frame + SCREENCAST_SCREEN_OFFSET, 1, SCREENCAST_SCREEN_SIZE_ST, f);
        }
        fclose(f);
    } else {
        logHdd(LOG_WARNING, "ScreencastAcsiCommand::frameInit - failed to load %s", logoPath.c_str());
    }
    pthread_mutex_unlock(&frameMutex);
}

size_t ScreencastAcsiCommand::screenSize(uint8_t res)
{
    switch(res) {
        case SCREENCAST_RES_TT_LOW:
        case SCREENCAST_RES_TT_MID:
        case SCREENCAST_RES_TT_HIGH:
            return SCREENCAST_SCREEN_SIZE_TT;
    }
    return SCREENCAST_SCREEN_SIZE_ST;               // ST resolutions, and the ones which are not supported yet - those send ST screen
}

size_t ScreencastAcsiCommand::frameSize(uint8_t res)
{
    return SCREENCAST_SCREEN_OFFSET + screenSize(res);
}

size_t ScreencastAcsiCommand::frameGet(uint8_t* out, uint32_t* seq)
{
    pthread_mutex_lock(&frameMutex);
    size_t size = frameSize(frame[0]);
    memcpy(out, frame, size);
    *seq = frameSeq;
    pthread_mutex_unlock(&frameMutex);
    return size;
}

size_t ScreencastAcsiCommand::frameChunk(const uint8_t* frm, size_t frmSize, uint32_t seq, size_t offset, uint8_t* out)
{
    if(offset >= frmSize) {
        return 0;
    }

    size_t len = MIN(SCREENCAST_CHUNK_PAYLOAD, frmSize - offset);

    Utils::storeDword(out, (uint32_t) offset);
    Utils::storeDword(out + 4, (uint32_t) frmSize);
    Utils::storeDword(out + 8, seq);
    memcpy(out + SCREENCAST_CHUNK_HEADER, frm + offset, len);

    return SCREENCAST_CHUNK_HEADER + len;
}

// when the web UI retrieved the last live frame (Utils::getCurrentMs()) - set by command socket thread, read by HDD thread
static std::atomic<bool>     liveOn(false);
static std::atomic<uint32_t> liveLastMs(0);

void ScreencastAcsiCommand::liveRequested(void)
{
    liveLastMs = Utils::getCurrentMs();
    liveOn = true;
}

void ScreencastAcsiCommand::liveStop(void)
{
    liveOn = false;
}

bool ScreencastAcsiCommand::liveActive(void)
{
    if(!liveOn) {
        return false;
    }

    if(Utils::elapsedMs(liveLastMs) >= SCREENCAST_LIVE_TIMEOUT_MS) {   // web UI stopped retrieving frames (page closed)? off
        liveOn = false;
        return false;
    }

    return true;
}

int ScreencastAcsiCommand::frameSkipForFps(int fps)
{
    fps = MAX(SCREENCAST_FPS_MIN, MIN(fps, SCREENCAST_FPS_MAX));
    return (50 + fps / 2) / fps;            // rounded 50 / fps (50 Hz VBL): 25 fps -> 2 VBLs, 1 fps -> 50 VBLs
}

void ScreencastAcsiCommand::processCommand(uint8_t *command)
{
    cmd = command;

    if(dataTrans == 0) {
        logHdd(LOG_ERROR, "ScreencastAcsiCommand::processCommand was called without valid dataTrans, can't tell ST that his went wrong...");
        return;
    }

    dataTrans->clear();                 // clean data transporter before handling

    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::processCommand");

    switch(cmd[4]) {
        case TRAN_CMD_SENDSCREENCAST:                    // ST sends screen buffer
            readScreen();
            break;
        case TRAN_CMD_SCREENCASTPALETTE:                    // ST sends palette
            readPalette();
            break;
    }

    //>dataTrans->sendDataAndStatus();         // send all the stuff after handling, if we got any
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand done.");
}

void ScreencastAcsiCommand::readScreen(void)
{
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen TRAN_CMD_SENDSCREENCAST");
    uint8_t iScreenmode = cmd[5];
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen screenmode %d",iScreenmode);

    uint32_t byteCount = Utils::get24bits(cmd + 6);
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen bytes %d",byteCount);

    // bigger screens (TT) come in parts, this is where this part goes in the screen; older drivers have zero there
    uint32_t screenOffset = Utils::get24bits(cmd + 9);
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen offset %d",screenOffset);

    // screenmode is SCREENCAST_RES_* - not checked here, it's just passed in the frame to the web UI, which shows what it knows

    if(byteCount > (SCREENCAST_BUFFER_SIZE - 16)) {                 // more than fits in dataBuffer (incl. padding to 16)? fail!
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen too many bytes %d, max is %d", byteCount, SCREENCAST_BUFFER_SIZE - 16);
        dataTrans->setStatus(EINTRN);
        return;
    }

    if(screenOffset + byteCount > SCREENCAST_SCREEN_SIZE_TT) {      // wouldn't fit in the frame? fail!
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen offset %d + bytes %d is out of screen, max is %d", screenOffset, byteCount, SCREENCAST_SCREEN_SIZE_TT);
        dataTrans->setStatus(EINTRN);
        return;
    }

    int mod = byteCount % 16;       // how many we got in the last 1/16th part?
    int pad = 0;

    if(mod != 0) {
        pad = 16 - mod;             // how many we need to add to make count % 16 equal to 0?
    }

    uint32_t transferSizeBytes = byteCount + pad;

    bool res;
    res = dataTrans->recvData(dataBuffer, transferSizeBytes);   // get data from Hans

    if(!res) {                                                  // failed to get data? internal error!
        dataTrans->setStatus(EINTRN);
        return;
    }

    pthread_mutex_lock(&frameMutex);
    memcpy(frame + SCREENCAST_SCREEN_OFFSET + screenOffset, dataBuffer, byteCount);  // screen data
    pthread_mutex_unlock(&frameMutex);

    dataTrans->setStatus(RW_ALL_TRANSFERED);                    // when all the data was written
}

void ScreencastAcsiCommand::readPalette(void)
{
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette TRAN_CMD_SCREENCASTPALETTE");
    uint8_t iScreenmode = cmd[5];
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette screenmode %d",iScreenmode);

    uint32_t byteCount = Utils::get24bits(cmd + 6);
    logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette bytes %d",byteCount);

    // screenmode is SCREENCAST_RES_* - not checked here, it's just passed in the frame to the web UI, which shows what it knows

    if(byteCount > SCREENCAST_PALETTE_SIZE) {                       // more than fits in frame palette? fail!
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette too many bytes %d, max is %d", byteCount, SCREENCAST_PALETTE_SIZE);
        dataTrans->setStatus(EINTRN);
        return;
    }

    int mod = byteCount % 16;       // how many we got in the last 1/16th part?
    int pad = 0;

    if(mod != 0) {
        pad = 16 - mod;             // how many we need to add to make count % 16 equal to 0?
    }

    uint32_t transferSizeBytes = byteCount + pad;

    bool res;
    res = dataTrans->recvData(dataBuffer, transferSizeBytes);   // get data from Hans

    if(!res) {                                                  // failed to get data? internal error!
        dataTrans->setStatus(EINTRN);
        return;
    }

    // special case monochrome palette - always white + black
    if(iScreenmode == SCREENCAST_RES_ST_HIGH) {
        dataBuffer[0] = dataBuffer[1] = 0xff;
        dataBuffer[2] = dataBuffer[3] = 0x00;
    }

    pthread_mutex_lock(&frameMutex);
    frame[0] = iScreenmode;                                     // 0: resolution - SCREENCAST_RES_* - palette comes last, so it marks the whole frame
    memcpy(frame + 1, dataBuffer, byteCount);                   // palette: 16 ST colors (32 B), up to 256 TT colors (512 B)
    frameSeq++;                                                 // whole frame arrived
    pthread_mutex_unlock(&frameMutex);

    dataTrans->setStatus(RW_ALL_TRANSFERED);                    // when all the data was written
}
