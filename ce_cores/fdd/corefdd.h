#ifndef COREFDD_H
#define COREFDD_H

#include "../misc/global.h"
#include "../misc/settings.h"
#include "../misc/utils.h"
#include "../chipinterface/chipinterface.h"

class ImageSilo;
class ChipInterface;

#define FDD_BUF_SIZE    16384

void *fddThreadCode(void *ptr);     // floppy core thread

class CoreFdd
{
public:
    CoreFdd();
    virtual ~CoreFdd();

    void run(void);
    virtual void reloadSettings(int type);                                  // from ISettingsUser
    bool handleOneClient(ClientInfo* ci);

private:
    bool shouldRun;
    bool running;

    ChipInterface* ciFdd;
    ImageSilo* imageSilo;

    //-----------------------------------
    // settings and config stuff

    void loadSettings(void);

    //-----------------------------------
    // floppy stuff

    bool handleFdd(ClientInfo* ci, uint8_t* inBuff);
    void handleFwVersion_franz(ClientInfo* ci);
    void handleSectorWritten(ClientInfo* ci);
    void handleSendTrack(ClientInfo* ci);
    void handleSendImageToClient(ClientInfo* client);

    void loadLastImageIntoSlot(ClientInfo* ci);


    bool parseMac(const std::string& macStr, uint8_t* mac);     // false if macStr isn't 12 hex chars
    void handleFddAction(int fddAction, const std::string& fddMac);

    //----------------------------------
    // recovery stuff
    void insertSpecialFloppyImage(int specialImageId);
};

#endif
