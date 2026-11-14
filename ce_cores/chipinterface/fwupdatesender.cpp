// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "fwupdatesender.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "../misc/settings.h"
#include "../misc/json.h"
#include "../misc/global.h"

using json = nlohmann::json;

static uint8_t noData[1];       // payload of the commands without data

FwUpdateSender::FwUpdateSender(ChipInterface* chipIf, int whichLog, uint16_t cmdStart, uint16_t cmdData, uint16_t cmdEnd, uint16_t cmdAbort)
{
    this->chipIf = chipIf;
    this->whichLog = whichLog;
    this->cmdStart = cmdStart;
    this->cmdData = cmdData;
    this->cmdEnd = cmdEnd;
    this->cmdAbort = cmdAbort;

    for(int i=0; i<MAX_CLIENTS; i++) {
        st[i].phase = PH_IDLE;
        st[i].fd = FD_EMPTY;
        memset(st[i].mac, 0, 6);
        memset(st[i].md5, 0, 16);
        st[i].sent = 0;
        st[i].chunkLen = 0;
        st[i].lastMs = 0;
        st[i].lastPercent = -1;
    }
}

bool FwUpdateSender::parseHex(const std::string& hex, uint8_t* out, int outLen)
{
    std::string h;
    for(char c : hex) {         // allow aa:bb:cc... MAC format too
        if(c != ':' && c != '-') {
            h += c;
        }
    }

    if((int) h.length() != outLen * 2) {
        return false;
    }

    for(int i=0; i<outLen; i++) {
        unsigned int v;
        char byte[3] = { h[i * 2], h[i * 2 + 1], 0 };

        if(!isxdigit((unsigned char) byte[0]) || !isxdigit((unsigned char) byte[1]) || sscanf(byte, "%2x", &v) != 1) {
            return false;
        }

        out[i] = (uint8_t) v;
    }

    return true;
}

bool FwUpdateSender::isRunning(int clientIndex)
{
    if(clientIndex < 0 || clientIndex >= MAX_CLIENTS) {
        return false;
    }

    int ph = st[clientIndex].phase;
    return ph == PH_WAIT_START || ph == PH_WAIT_DATA || ph == PH_WAIT_END;
}

void FwUpdateSender::writeStatus(const uint8_t* mac, const char* state, uint32_t sent, uint32_t total, const std::string& message)
{
    int progress = (total > 0) ? (int) (((uint64_t) sent * 100) / total) : 0;

    json j = {
        {"state", state},
        {"progress", progress},
        {"sent", sent},
        {"total", total},
        {"message", message},
        {"time", (long long) time(NULL)}
    };

    Settings s((uint8_t*) mac);
    s.setString(FW_UPDATE_STATUS_KEY, j.dump().c_str());
}

void FwUpdateSender::handleRequest(const std::string& macStr, const std::string& file, const std::string& md5hex, bool abort)
{
    uint8_t mac[6];
    if(!parseHex(macStr, mac, 6)) {
        Debug::out(whichLog, LOG_WARNING, "FwUpdateSender::handleRequest - invalid mac '%s'", macStr.c_str());
        return;
    }

    ClientInfo* ci = chipIf->clientsGetOneByMac(mac);

    if(abort) {
        if(ci && isRunning(ci->index)) {
            chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdAbort, noData, 0);
            finish(ci->index, "aborted", "aborted by user");
        }
        return;
    }

    if(!ci) {
        writeStatus(mac, "failed", 0, 0, "device is not connected");
        return;
    }

    if(isRunning(ci->index)) {      // keep the status of the running one
        Debug::out(whichLog, LOG_WARNING, "FwUpdateSender - update of this device is already running, request ignored");
        return;
    }

    std::string path = file;
    if(path.empty()) {
        char macHex[13];
        Settings::binToHex(mac, 6, macHex);
        path = Utils::mergeHostPaths2(Utils::mergeHostPaths2(Utils::dotEnvValue("SETTINGS_DIR", "./settings"), std::string(macHex)), std::string(FW_UPDATE_BIN_KEY));
    }

    std::string err;
    if(!start(ci, path, md5hex, err)) {
        Debug::out(whichLog, LOG_WARNING, "FwUpdateSender - update not started: %s", err.c_str());
        writeStatus(mac, "failed", 0, 0, err);
    }
}

bool FwUpdateSender::start(ClientInfo* ci, const std::string& file, const std::string& md5hex, std::string& err)
{
    State& s = st[ci->index];

    if(isRunning(ci->index)) {
        err = "update of this device is already running";
        return false;
    }

    uint8_t md5[16];
    if(!parseHex(md5hex, md5, 16)) {
        err = "invalid MD5";
        return false;
    }

    // read the whole image
    FILE* f = fopen(file.c_str(), "rb");
    if(!f) {
        err = "can't open the firmware file";
        return false;
    }

    std::vector<uint8_t> image;
    uint8_t bfr[4096];
    size_t n;
    while((n = fread(bfr, 1, sizeof(bfr), f)) > 0 && image.size() <= FWU_MAX_IMAGE_SIZE) {
        image.insert(image.end(), bfr, bfr + n);
    }
    fclose(f);

    if(image.empty() || image.size() > FWU_MAX_IMAGE_SIZE) {
        err = "firmware file is empty or too big";
        return false;
    }

    // START: size, MD5
    uint8_t cmd[20];
    Utils::storeDword(cmd, image.size());
    memcpy(cmd + 4, md5, 16);

    if(!chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdStart, cmd, sizeof(cmd))) {
        err = "sending to device failed";
        return false;
    }

    s.phase = PH_WAIT_START;
    s.fd = ci->fdClient;
    memcpy(s.mac, ci->mac, 6);
    s.image.swap(image);
    memcpy(s.md5, md5, 16);
    s.sent = 0;
    s.chunkLen = 0;
    s.lastMs = Utils::getCurrentMs();
    s.lastPercent = 0;

    Debug::out(whichLog, LOG_INFO, "FwUpdateSender - starting update of %02X:%02X:%02X:%02X:%02X:%02X with %s, %u B",
        s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], file.c_str(), (unsigned) s.image.size());

    writeStatus(s.mac, "starting", 0, s.image.size(), "");
    return true;
}

bool FwUpdateSender::sendNextChunk(ClientInfo* ci)
{
    State& s = st[ci->index];

    uint32_t size = s.image.size();
    s.chunkLen = (size - s.sent > FWU_CHUNK_SIZE) ? FWU_CHUNK_SIZE : (size - s.sent);

    uint8_t bfr[4 + FWU_CHUNK_SIZE];
    Utils::storeDword(bfr, s.sent);
    memcpy(bfr + 4, s.image.data() + s.sent, s.chunkLen);

    s.lastMs = Utils::getCurrentMs();
    return chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdData, bfr, 4 + s.chunkLen);
}

void FwUpdateSender::finish(int idx, const char* state, const std::string& message)
{
    State& s = st[idx];

    Debug::out(whichLog, strcmp(state, "rebooting") == 0 ? LOG_INFO : LOG_WARNING, "FwUpdateSender - update %s at %u of %u B %s",
        state, s.sent, (unsigned) s.image.size(), message.c_str());

    writeStatus(s.mac, state, s.sent, s.image.size(), message);

    s.phase = (strcmp(state, "rebooting") == 0) ? PH_REBOOTING : PH_IDLE;
    std::vector<uint8_t>().swap(s.image);       // free the memory
}

void FwUpdateSender::onStatus(ClientInfo* ci)
{
    uint8_t bfr[128];
    int cnt = chipIf->readRestOfData(ci, bfr, sizeof(bfr) - 1);
    bfr[sizeof(bfr) - 1] = 0;

    if(cnt < 6) {
        Debug::out(whichLog, LOG_ERROR, "FwUpdateSender::onStatus - too short: %d", cnt);
        return;
    }

    uint8_t  cmd      = bfr[0];
    uint8_t  result   = bfr[1];
    uint32_t received = Utils::getDword(bfr + 2);
    std::string msg   = (cnt > 6) ? std::string((const char*) bfr + 6, strnlen((const char*) bfr + 6, cnt - 6)) : "";

    int idx = ci->index;
    State& s = st[idx];

    if(!isRunning(idx) || s.fd != ci->fdClient) {
        Debug::out(whichLog, LOG_DEBUG, "FwUpdateSender::onStatus - cmd %02X, result %d, no update running - ignored", cmd, result);
        return;
    }

    if(result != 0) {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "device refused: %s (error %d)", msg.c_str(), result);
        finish(idx, "failed", tmp);
        return;
    }

    if(cmd == (uint8_t) cmdStart && s.phase == PH_WAIT_START) {
        s.phase = PH_WAIT_DATA;

        if(!sendNextChunk(ci)) {
            finish(idx, "failed", "sending to device failed");
        }
        return;
    }

    if(cmd == (uint8_t) cmdData && s.phase == PH_WAIT_DATA) {
        if(received != s.sent + s.chunkLen) {
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "device has %u B, expected %u B", received, s.sent + s.chunkLen);
            chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdAbort, noData, 0);
            finish(idx, "failed", tmp);
            return;
        }

        s.sent = received;

        if(s.sent >= s.image.size()) {          // all sent? let the device check it and reboot
            s.phase = PH_WAIT_END;
            s.lastMs = Utils::getCurrentMs();
            writeStatus(s.mac, "finishing", s.sent, s.image.size(), "");

            if(!chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdEnd, noData, 0)) {
                finish(idx, "failed", "sending to device failed");
            }
            return;
        }

        int percent = (int) (((uint64_t) s.sent * 100) / s.image.size());
        if(percent != s.lastPercent) {          // don't write the status file for each chunk
            s.lastPercent = percent;
            writeStatus(s.mac, "sending", s.sent, s.image.size(), "");
        }

        if(!sendNextChunk(ci)) {
            finish(idx, "failed", "sending to device failed");
        }
        return;
    }

    if(cmd == (uint8_t) cmdEnd && s.phase == PH_WAIT_END) {
        finish(idx, "rebooting", "");
        return;
    }

    Debug::out(whichLog, LOG_WARNING, "FwUpdateSender::onStatus - unexpected answer to cmd %02X in phase %d", cmd, s.phase);
}

void FwUpdateSender::tick(void)
{
    for(int i=0; i<MAX_CLIENTS; i++) {
        if(!isRunning(i)) {
            continue;
        }

        State& s = st[i];
        ClientInfo* ci = chipIf->clientGetByIndex(i);

        if(!ci || ci->fdClient != s.fd || memcmp(ci->mac, s.mac, 6) != 0) {
            finish(i, "failed", "connection to device lost");
            continue;
        }

        if(Utils::elapsedMs(s.lastMs) > FWU_ANSWER_TIMEOUT_MS) {
            chipIf->sendHeaderAndDataToChip(ci->fdClient, cmdAbort, noData, 0);
            finish(i, "failed", "device didn't answer");
        }
    }
}
