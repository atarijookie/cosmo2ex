#ifndef COREFDD_H
#define COREFDD_H

#include "../misc/global.h"
#include "../misc/settings.h"
#include "../misc/utils.h"
#include "../chipinterface/chipinterface.h"
#include "imagelist.h"

class ImageSilo;
class ChipInterface;

#define FDD_BUF_SIZE    16384

#define IMAGE_LIST_CHECK_MS     1000    // how often the browsed folders are checked for changes

// browsing the images in the folder of the current image, per connected client (by client index)
typedef struct {
    bool        wanted;         // device asked for the list - only then it gets the changes
    uint32_t    start;          // the page it asked for last, resent on changes (IMAGE_LIST_NONE = around the current image)
    uint16_t    count;
    ImageList   list;
} ImageBrowse;

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

    //-----------------------------------
    // browsing the images on the device
    ImageBrowse browse[MAX_CLIENTS];
    uint32_t lastImageListCheck;

    void handleGetImageList(ClientInfo* ci);
    void handleInsertImage(ClientInfo* ci);
    void imageBrowseReset(int clientIndex);
    void updateImageListFolder(ClientInfo* ci, std::string& fileName);  // folder of the current image to the list, returns the image file name
    void sendImageList(ClientInfo* ci);
    void checkImageLists(void);                 // resend the lists which changed on disk

    void loadLastImageIntoSlot(ClientInfo* ci);


    bool parseMac(const std::string& macStr, uint8_t* mac);     // false if macStr isn't 12 hex chars
    void handleFddAction(int fddAction, const std::string& fddMac);

    //----------------------------------
    // recovery stuff
    void insertSpecialFloppyImage(int specialImageId);
};

#endif
