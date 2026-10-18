#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

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
static uint8_t frame[SCREENCAST_FRAME_SIZE];
static pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;

void ScreencastAcsiCommand::frameInit(void)
{
    pthread_mutex_lock(&frameMutex);
    memset(frame, 0, sizeof(frame));                // set all to zeros

    std::string logoPath = Utils::dotEnvValue("CE_LOGO_FILE", "ce_logo.bin");   // path to logo file
    FILE *f = fopen(logoPath.c_str(), "rb");        // if can open this logo file
    if(f) {
        fread(frame + 1, 1, sizeof(frame) - 1, f);  // fill the initial frame with logo - palette and screen data
        fclose(f);
    } else {
        logHdd(LOG_WARNING, "ScreencastAcsiCommand::frameInit - failed to load %s", logoPath.c_str());
    }
    pthread_mutex_unlock(&frameMutex);
}

void ScreencastAcsiCommand::frameGet(uint8_t* out)
{
    pthread_mutex_lock(&frameMutex);
    memcpy(out, frame, sizeof(frame));
    pthread_mutex_unlock(&frameMutex);
}

size_t ScreencastAcsiCommand::frameChunk(const uint8_t* frm, size_t offset, uint8_t* out)
{
    if(offset >= SCREENCAST_FRAME_SIZE) {
        return 0;
    }

    size_t len = MIN(SCREENCAST_CHUNK_PAYLOAD, SCREENCAST_FRAME_SIZE - offset);

    Utils::storeWord(out, (uint16_t) offset);
    Utils::storeWord(out + 2, (uint16_t) SCREENCAST_FRAME_SIZE);
    memcpy(out + SCREENCAST_CHUNK_HEADER, frm + offset, len);

    return SCREENCAST_CHUNK_HEADER + len;
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

    if(iScreenmode<0 || iScreenmode>2) {                       // unknown screenmode?
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen unknown screenmode %d",iScreenmode);
        dataTrans->setStatus(E_NOTHANDLED);
        return;
    }

    if(byteCount > (SCREENCAST_BUFFER_SIZE - 16)) {                 // more than fits in dataBuffer (incl. padding to 16)? fail!
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readScreen too many bytes %d, max is %d", byteCount, SCREENCAST_BUFFER_SIZE - 16);
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
    memcpy(frame + 33, dataBuffer, 32000);                      // 33 - 32032: screen data
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

    if(iScreenmode<0 || iScreenmode>2) {                       // unknown screenmode?
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette unknown screenmode %d",iScreenmode);
        dataTrans->setStatus(E_NOTHANDLED);
        return;
    }

    if(byteCount > (SCREENCAST_BUFFER_SIZE - 16)) {                 // more than fits in dataBuffer (incl. padding to 16)? fail!
        logHdd(LOG_DEBUG, "ScreencastAcsiCommand::readPalette too many bytes %d, max is %d", byteCount, SCREENCAST_BUFFER_SIZE - 16);
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
    if(iScreenmode == 2) {
        dataBuffer[0] = dataBuffer[1] = 0xff;
        dataBuffer[2] = dataBuffer[3] = 0x00;
    }

    pthread_mutex_lock(&frameMutex);
    frame[0] = iScreenmode;                                     // 0: resolution
    memcpy(frame + 1, dataBuffer, 32);                          // 1-32: palette
    pthread_mutex_unlock(&frameMutex);

    dataTrans->setStatus(RW_ALL_TRANSFERED);                    // when all the data was written
}
