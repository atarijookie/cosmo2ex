#ifndef HDDCLIENT_H
#define HDDCLIENT_H

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../misc/global.h"
#include "../misc/settings.h"
#include "../misc/version.h"
#include "../misc/utils.h"
#include "../chipinterface/chipinterface.h"

class AcsiDataTrans;
class RetryModule;
class ChipInterfaceClient;
class ChipInterface;
class TranslatedDisk;
class Scsi;
class SlmPrinter;
class NetAdapter;
class BrowserModule;
class MediaModule;

class HddClient
{
public:
    HddClient(ChipInterface* cin, ClientInfo* ci);
    virtual ~HddClient();

    bool handleHdd(uint8_t* inBuff);
    void reloadDrivesRaw(void);
    void reloadDrivesTranslated(void);

    void sendHalfWord(void);

    void onMacUpdated(void);
    void onClientConnected(void);

private:

    ChipInterface* chipInterface;
    ClientInfo* ci;

    THwConfig hwConfig;
    Scsi* scsi;
    TranslatedDisk* translated;
    AcsiDataTrans* dataTrans;
    SlmPrinter* slm;                // SLM laser printer emulation - for ACSI IDs set to DEVTYPE_SLM*
    NetAdapter* netAdapter;         // network module of the fake STinG (ce_fakesting) - HOSTMOD_NETWORK_ADAPTER
    BrowserModule* browser;         // web browser (CE_BROWS.PRG) - HOSTMOD_BROWSER
    MediaModule* media;             // video / audio player (CE_VIDEO.TTP, CE_AUDIO.TTP) - HOSTMOD_MEDIA_STREAMING

    //-----------------------------------
    // hard disk stuff
    RetryModule* retryMod;

    void handleAcsiCommand(uint8_t *bufIn);
    void handleSlmCommand(uint8_t *bufIn);
    bool slmWaitForCommand(uint32_t timeoutMs, uint8_t* cmd);

    //-----------------------------------
    // handle FW version
    void handleFwVersion_hans(void);

    void saveHwConfig(void);
    void resetHwConfig(void);
    uint8_t getIdBits(void);

    //----------------------------------
    // other
    void extractInterfaceInfo(uint8_t xilinxInfo);
};

#endif
