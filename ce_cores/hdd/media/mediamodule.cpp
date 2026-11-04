#include <stdio.h>
#include <string.h>

#include "../../misc/global.h"
#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "../acsidatatrans.h"
#include "../translated/translateddisk.h"
#include "media_commands.h"
#include "mediamodule.h"

MediaModule::MediaModule(AcsiDataTrans *dt, TranslatedDisk *translated)
{
    cmd       = NULL;
    dataTrans = dt;
    nextGen   = 1;

    resolvePath = [translated](const std::string& atariPath, std::string& hostPath) {
        if(!translated) {
            return false;
        }
        std::string fullAtariPath;
        int driveIndex;
        bool waitingForMount;
        if(!translated->createFullAtariPathAndFullHostPath(atariPath, fullAtariPath, driveIndex, hostPath, waitingForMount) ||
           hostPath.empty()) {
            return false;
        }
        return translated->hostPathExists(hostPath, true);  // also finds the file when the case differs (and fixes hostPath)
    };
}

MediaModule::~MediaModule()
{
}

void MediaModule::reset(void)
{
    link.stop();
}

void MediaModule::processCommand(uint8_t *command)
{
    cmd = command;

    if(!dataTrans) {
        logHdd(LOG_ERROR, "MediaModule::processCommand - no dataTrans");
        return;
    }

    dataTrans->clear();
    if(cmd[4] != MED_CMD_GET_CHUNK) {                               // that one comes many times a second
        logHdd(LOG_DEBUG, "MediaModule::processCommand - %s", cmdName(cmd[4]));
    }
    link.noteActivity();

    switch(cmd[4]) {
        case MED_CMD_IDENTIFY:      identify();     break;
        case MED_CMD_OPEN:          open();         break;
        case MED_CMD_GET_INFO:      getInfo();      break;
        case MED_CMD_GET_YM_TABLE:  getYmTable();   break;
        case MED_CMD_CLOSE:         close();        break;
        case MED_CMD_GET_CHUNK:     getChunk();     break;
        case MED_CMD_SEEK:          seek();         break;
        case MED_CMD_INPUT:         input();        break;
        default:
            logHdd(LOG_WARNING, "MediaModule - unknown command 0x%02x", cmd[4]);
            dataTrans->setStatus(MED_ERR_BAD_PARAM);
            break;
    }

    dataTrans->sendDataAndStatus();
}

//--------------------------------------------------------

void MediaModule::identify(void)
{
    if(!link.start()) {                                             // CE_VIDEO / CE_AUDIO started - get ce_media going
        logHdd(LOG_ERROR, "MediaModule::identify - can't listen for ce_media");
    }

    dataTrans->addDataBfr("CosmosEx media module", 22, false);     // 21 chars + terminator
    dataTrans->addZerosUntilSize(32);
    dataTrans->addDataWord(MEDIA_MODULE_VERSION);                   // 32
    dataTrans->addDataByte(link.isConnected() ? MED_IDF_CONNECTED : 0);    // 34
    dataTrans->addZerosUntilSize(512);
    dataTrans->setStatus(MED_OK);
}

// data: 512 B - MED_OPEN_* header + Atari path
void MediaModule::open(void)
{
    uint8_t buf[MED_OPEN_SIZE];
    if(!dataTrans->recvData(buf, sizeof(buf))) {
        dataTrans->setStatus(MED_ERR_INTERNAL);
        return;
    }
    buf[sizeof(buf) - 1] = 0;

    std::string atariPath((const char *) buf + MED_OPEN_PATH);
    uint8_t vmode = buf[MED_OPEN_VMODE];
    uint8_t aout  = buf[MED_OPEN_AOUT];
    if(atariPath.empty() || vmode > MED_VMODE_TT_HIGH_WIN || aout > MED_AOUT_YM || buf[MED_OPEN_MACHINE] > MED_MACH_FALCON) {
        logHdd(LOG_WARNING, "MediaModule::open - bad parameters (vmode %d, aout %d)", (int) vmode, (int) aout);
        dataTrans->setStatus(MED_ERR_BAD_PARAM);
        return;
    }

    link.close();                                                   // the previous stream ends, whatever happens next

    bool game = (buf[MED_OPEN_FLAGS] & MED_OPF_GAME) != 0;
    std::string hostPath;
    if(game) {                                                      // a game on the host - its name goes as it is
        hostPath = atariPath;
    } else if(!resolvePath(atariPath, hostPath)) {
        logHdd(LOG_INFO, "MediaModule::open - %s is not on a translated drive, or doesn't exist", atariPath.c_str());
        dataTrans->setStatus(MED_ERR_NOT_FOUND);
        return;
    }

    if(!link.start()) {
        dataTrans->setStatus(MED_ERR_NOT_RUNNING);
        return;
    }

    uint32_t gen = nextGen++;
    bool live = (buf[MED_OPEN_FLAGS] & MED_OPF_LIVE) != 0;
    link.open(gen, buf, game ? hostPath : Utils::pathToUi(hostPath), live);   // Windows: ce_media is a Windows program - C:\... path

    logHdd(LOG_INFO, "MediaModule::open - %s -> %s, machine %d, vmode %d, audio %d, latency %d ms, %d sectors%s",
           atariPath.c_str(), hostPath.c_str(), (int) buf[MED_OPEN_MACHINE], (int) vmode, (int) aout,
           (int) Utils::getWord(buf + MED_OPEN_LATENCY), (int) buf[MED_OPEN_MAX_SECTORS], live ? ", live" : "");
    dataTrans->setStatus(MED_OK);
}

void MediaModule::getInfo(void)
{
    std::vector<uint8_t> b = link.info();
    dataTrans->addDataBfr(b.data(), b.size(), false);
    dataTrans->setStatus(MED_OK);
}

void MediaModule::getYmTable(void)
{
    std::vector<uint8_t> t = link.ymTable();
    if(t.empty()) {
        dataTrans->setStatus(MED_ERR_NO_STREAM);
        return;
    }
    dataTrans->addDataBfr(t.data(), t.size(), false);
    dataTrans->setStatus(MED_OK);
}

void MediaModule::close(void)
{
    link.close();
    dataTrans->setStatus(MED_OK);
}

// [5] max sectors, [6..9] generation
void MediaModule::getChunk(void)
{
    uint32_t maxBytes = (uint32_t) MAX(1, MIN((int) cmd[5], ACSI_MAX_TRANSFER_SIZE_BYTES / 512)) * 512;
    uint32_t gen      = Utils::getDword(cmd + 6);

    if(!link.hasStream()) {
        dataTrans->setStatus(MED_ERR_NO_STREAM);
        return;
    }

    MediaChunk c = link.popChunk(gen);
    if(c && c->size() > maxBytes) {                                 // the ST said less in OPEN than now
        logHdd(LOG_WARNING, "MediaModule::getChunk - chunk of %d bytes, the ST takes %d - dropped", (int) c->size(), (int) maxBytes);
        dataTrans->setStatus(MED_ERR_BAD_PARAM);
        return;
    }

    if(!c) {                                                        // nothing yet (or the ST asks for an old generation)
        uint8_t h[MED_CHUNK_HEADER_SIZE];
        memset(h, 0, sizeof(h));
        Utils::storeDword(h + MED_CHUNK_GENERATION, link.generation());
        h[MED_CHUNK_FLAGS] = MED_CHF_EMPTY | (link.ended() ? MED_CHF_END : 0);
        dataTrans->addDataBfr(h, sizeof(h), false);
        dataTrans->setStatus(MED_OK);
        return;
    }

    dataTrans->addDataBfr(c->data(), c->size(), true);
    dataTrans->setStatus(MED_OK);
}

// [5..8] position ms. Reply: [0..3] new generation, [4..7] position it starts at
void MediaModule::seek(void)
{
    if(!link.hasStream()) {
        dataTrans->setStatus(MED_ERR_NO_STREAM);
        return;
    }

    uint32_t posMs = Utils::getDword(cmd + 5);
    std::vector<uint8_t> info = link.info();
    uint32_t durMs = Utils::getDword(&info[MED_INFO_DURATION_MS]);
    if(durMs > 0 && posMs + 1000 > durMs) {                         // not past the end - the last second at most
        posMs = (durMs > 1000) ? durMs - 1000 : 0;
    }

    uint32_t gen = nextGen++;
    link.seek(gen, posMs);
    logHdd(LOG_INFO, "MediaModule::seek - %u ms, generation %u", posMs, gen);

    dataTrans->addDataDword(gen);
    dataTrans->addDataDword(posMs);
    dataTrans->addZerosUntilSize(512);
    dataTrans->setStatus(MED_OK);
}

// [5] count, [6..11] scancodes
void MediaModule::input(void)
{
    int count = cmd[5];
    if(count > 6) {
        dataTrans->setStatus(MED_ERR_BAD_PARAM);
        return;
    }
    if(!link.hasStream()) {
        dataTrans->setStatus(MED_ERR_NO_STREAM);
        return;
    }
    if(count > 0 && !link.input(cmd + 6, count)) {
        dataTrans->setStatus(MED_ERR_NOT_RUNNING);
        return;
    }
    dataTrans->setStatus(MED_OK);
}

const char *MediaModule::cmdName(uint8_t c)
{
    switch(c) {
        case MED_CMD_IDENTIFY:      return "IDENTIFY";
        case MED_CMD_OPEN:          return "OPEN";
        case MED_CMD_GET_INFO:      return "GET_INFO";
        case MED_CMD_GET_YM_TABLE:  return "GET_YM_TABLE";
        case MED_CMD_CLOSE:         return "CLOSE";
        case MED_CMD_GET_CHUNK:     return "GET_CHUNK";
        case MED_CMD_SEEK:          return "SEEK";
        case MED_CMD_INPUT:         return "INPUT";
    }
    return "unknown";
}
