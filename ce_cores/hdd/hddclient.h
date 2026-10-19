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

    //-----------------------------------
    // hard disk stuff
    RetryModule* retryMod;

    void handleAcsiCommand(uint8_t *bufIn);

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
