#ifndef __FWUPDATESENDER_H__
#define __FWUPDATESENDER_H__

#include <stdint.h>
#include <string>
#include <vector>

#include "chipinterface.h"

/*
    Firmware update (OTA) of a device over its existing connection - the web UI checks the firmware file (UF2 -> .bin,
    identity, chip, layout), stores the .bin and asks ce_cores via the command socket (fwUpdateRequestPush()) to send it.

    The webserver stores the image as FW_UPDATE_BIN in the device's settings dir, so no path goes over the socket
    (the webserver and ce_cores see paths differently on Windows).

    Protocol (the device side: ce_pico_hdd/fw_update.cpp):
        host:   CMD_FW_UPDATE_START  size (4 B), MD5 of the image (16 B)
        device: ATN_FW_UPDATE_STATUS cmd (1 B), result (1 B), received (4 B), message (zero terminated)
        host:   CMD_FW_UPDATE_DATA   offset (4 B), data (max FWU_CHUNK_SIZE)    - the next one only after the device answered
        ...
        host:   CMD_FW_UPDATE_END    -> device checks the identity, layout, MD5, answers, reboots; the OTA stub copies the image
        host:   CMD_FW_UPDATE_ABORT  any time

    The progress goes to the device's settings dir, key FW_UPDATE_STATUS (json), for the web UI:
        {"state": "starting|sending|finishing|rebooting|failed|aborted", "progress": 0..100, "sent": B, "total": B,
         "message": "...", "time": unix time}
    After "rebooting" the device reconnects and reports its new firmware (FW_IMAGE_MD5, ...) - the web UI compares it.
*/

#define FWU_CHUNK_SIZE          1024            // must be <= FWU_CHUNK_MAX of the device
#define FWU_ANSWER_TIMEOUT_MS   15000           // device must answer each command within this time
#define FWU_MAX_IMAGE_SIZE      (4 * 1024 * 1024)

#define FW_UPDATE_STATUS_KEY    "FW_UPDATE_STATUS"
#define FW_UPDATE_BIN_KEY       "FW_UPDATE_BIN"         // the checked image (.bin), written by the webserver

class FwUpdateSender
{
public:
    // cmd* - the command codes of this device type (HDD: CMD_FW_UPDATE_*)
    FwUpdateSender(ChipInterface* chipIf, int whichLog, uint16_t cmdStart, uint16_t cmdData, uint16_t cmdEnd, uint16_t cmdAbort);

    // from the web UI; file "" = FW_UPDATE_BIN_KEY in the device's settings dir
    void handleRequest(const std::string& macStr, const std::string& file, const std::string& md5hex, bool abort);
    void onStatus(ClientInfo* ci);  // ATN_FW_UPDATE_STATUS arrived from this client, reads its payload
    void tick(void);                // call from the core loop - timeouts, lost connections

    bool isRunning(int clientIndex);

    static bool parseHex(const std::string& hex, uint8_t* out, int outLen);     // false if not exactly outLen bytes of hex

private:
    enum { PH_IDLE = 0, PH_WAIT_START, PH_WAIT_DATA, PH_WAIT_END, PH_REBOOTING };

    struct State {
        int         phase;
        int         fd;             // the connection the update runs on - another one in this slot = connection lost
        uint8_t     mac[6];
        std::vector<uint8_t> image;
        uint8_t     md5[16];
        uint32_t    sent;           // bytes confirmed by the device
        uint32_t    chunkLen;       // size of the chunk waiting for confirmation
        uint32_t    lastMs;         // when the last command was sent
        int         lastPercent;    // last progress written to the status
    };

    ChipInterface*  chipIf;
    int             whichLog;
    uint16_t        cmdStart, cmdData, cmdEnd, cmdAbort;
    State           st[MAX_CLIENTS];

    bool start(ClientInfo* ci, const std::string& file, const std::string& md5hex, std::string& err);
    bool sendNextChunk(ClientInfo* ci);
    void finish(int idx, const char* state, const std::string& message);       // update ended (failed / aborted / rebooting)
    void writeStatus(const uint8_t* mac, const char* state, uint32_t sent, uint32_t total, const std::string& message);
};

#endif
