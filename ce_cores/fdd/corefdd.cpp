#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>

#include "../misc/global.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "../chipinterface/chipinterface.h"
#include "../discovery/discovery.h"
#include "../../libdospath/libdospath.h"

#include "corefdd.h"
#include "imagesilo.h"
#include "floppyencoder.h"

#define BFR_SIZE                32      // buffer size for the small requests from device (send track, send whole image)
#define INSERT_BFR_SIZE         (9 + IMAGE_LIST_NAME_MAX)   // ATN_INSERT_IMAGE request: gen, index, name len, name

extern DebugVars    dbgVars;

extern volatile uint8_t slotSendToDevice[SLOT_COUNT];

void *fddThreadCode(void *ptr)
{
    CoreFdd *coreFdd;
    pthread_t floppyEncThreadInfo;

    logFdd(LOG_INFO, "Cosmo2ex FDD core starting at port %d", SERVER_TCP_PORT_FDD);
    printf("\nCosmo2ex FDD core starting at port %d\n", SERVER_TCP_PORT_FDD);
    printf("\nlog file: %s\n", Debug::getCoreLogFileName(false, LOGFILE_FDD));


    //-------------
    coreFdd = new CoreFdd();

    handlePthreadCreate("floppy encoder", &floppyEncThreadInfo, floppyEncodeThreadCode);

    coreFdd->run();                // run the fdd code
    delete coreFdd;

    floppyEncoder_stop();
    pthread_kill_join("floppy encoder", floppyEncThreadInfo);

    logFdd(LOG_INFO, "Cosmo2ex terminated.");
    printf("Terminated\n");
    return NULL;
}

CoreFdd::CoreFdd()
{
    lastImageListCheck = 0;

    for(int i=0; i<MAX_CLIENTS; i++) {
        imageBrowseReset(i);
    }
}

CoreFdd::~CoreFdd()
{

}

void CoreFdd::run(void)
{
    loadSettings();

    // create network chip interface
    ciFdd = new ChipInterface(LOGFILE_FDD, NET_ATN_FRANZ_ID, SYNC_TAG_FDD, SERVER_TCP_PORT_FDD);
    if(!ciFdd->ciOpen()) {                // can't listen on our tcp port? we can't serve any data, so terminate the app
        fatalErrorOccurred = 1;
        sigintReceived = 1;
    }

    imageSilo = new ImageSilo(ciFdd);

    int max_fd;
    fd_set readfds;

    while(sigintReceived == 0) {
        Utils::sleepMs(1);      // intentional sleep to not utilize cpu to max when looping too much before client disconnect

        // handle floppy actions from command socket
        int fddAction;
        std::string fddMac;
        while(fddActionPop(fddAction, fddMac)) {
            handleFddAction(fddAction, fddMac);
        }

        ciFdd->clientsDisconnectInactive();

        if(Utils::elapsedMs(lastImageListCheck) >= IMAGE_LIST_CHECK_MS) {
            lastImageListCheck = Utils::getCurrentMs();
            checkImageLists();              // images added / deleted / renamed in the browsed folders? resend the lists
        }

        // check if any slot was just loaded and needs to be sent to device
        for(int i=0; i<SLOT_COUNT; i++) {
            if(slotSendToDevice[i]) {                   // if slot #i should be sent to device
                slotSendToDevice[i] = false;

                ClientInfo* client = ciFdd->clientsGetOneByFloppySlot(i);    // slot # doesn't have to match client index, find client using this slot

                if(!client) {                           // no connected client uses this slot? skip
                    continue;
                }

                logFdd(LOG_DEBUG, "CoreFdd::run - sending whole image of slot %d to client %d", i, client->index);
                handleSendImageToClient(client);
            }
        }

        max_fd = -1;
        FD_ZERO(&readfds);

        // get listening socket from chip interface, add it to readfds
        int fdListen = ciFdd->getFdListen();
        FD_SET(fdListen, &readfds);
        max_fd = MAX(max_fd, fdListen);

        // get all connected client fds, add them to readfds
        max_fd = MAX(max_fd, ciFdd->setAllClientFds(&readfds));     // all valid client fds will be set to readfds, and highest fd into max_fd

        // add timeout to select(), so we can check for connection status, settings reload, etc.
        timeval timeout;
        memset(&timeout, 0, sizeof(timeout));
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;

        if(select(max_fd + 1, &readfds, NULL, NULL, &timeout) < 0) {
            if(errno == EINTR) {
                continue;   // a signal was delivered
            } else {
                logFdd(LOG_ERROR, "CoreFdd::run() select: %s", strerror(errno));
                continue;
            }
        }

        // if listening socket is set, handle it
        if(FD_ISSET(fdListen, &readfds)) {
            int idx = ciFdd->acceptSocketIfNeededAndPossible();

            if(idx != FD_EMPTY) {
                imageBrowseReset(idx);      // new client on this index - doesn't browse until it asks
                loadLastImageIntoSlot(ciFdd->clientGetByIndex(idx));
            }
        }

        // check which fds are ready to be handled and handle them
        for(int i=0; i<MAX_CLIENTS; i++) {
            ClientInfo* ci = ciFdd->clientGetByIndex(i);

            if(ci->fdClient == FD_EMPTY) {           // no client here? skip it
                continue;
            }

            if(FD_ISSET(ci->fdClient, &readfds)) {    // this fd read for read?
                if(handleOneClient(ci)) {
                    ci->lastMs = Utils::getCurrentMs();
                }
            }
        }
    }

    ciFdd->ciClose();
    delete ciFdd;
    ciFdd = NULL;

    delete imageSilo;
    imageSilo = NULL;
}

bool CoreFdd::handleOneClient(ClientInfo* ci)
{
    uint8_t inBuff[FDD_BUF_SIZE];
    memset(inBuff, 0, FDD_BUF_SIZE);

    bool needsAction = ciFdd->actionNeeded(ci, inBuff);

    if(needsAction) {   // floppy drive needs action?
        handleFdd(ci, inBuff);
    }

    return needsAction;
}

bool CoreFdd::handleFdd(ClientInfo* ci, uint8_t* inBuff)
{
    bool isFddCommand = false;
    uint8_t inBuf[BFR_SIZE];

    switch(inBuff[3]) {
    case ATN_FW_VERSION:                    // device has sent FW version
        handleFwVersion_franz(ci);
        break;

    case ATN_SECTOR_WRITTEN:                // device has sent written sector data
        isFddCommand = true;
        handleSectorWritten(ci);
        break;

    case ATN_SEND_TRACK:                    // device requests data of a whole track
        isFddCommand = true;
        handleSendTrack(ci);
        break;

    case ATN_SEND_WHOLE_IMAGE:
        isFddCommand = true;
        ciFdd->readRestOfData(ci, inBuf, BFR_SIZE);
        handleSendImageToClient(ci);
        break;

    case ATN_GET_IMAGE_LIST:
        handleGetImageList(ci);
        break;

    case ATN_INSERT_IMAGE:
        handleInsertImage(ci);
        break;

    default:
        logFdd(LOG_ERROR, "CoreFdd received weird ATN code %02x waitForATN()", inBuff[3]);
        break;
    }

    return isFddCommand;
}

void CoreFdd::reloadSettings(int type)
{
    // then load the new settings
    loadSettings();
}

void CoreFdd::loadSettings(void)
{
    logFdd(LOG_DEBUG, "CoreFdd::loadSettings");
}

void CoreFdd::loadLastImageIntoSlot(ClientInfo* ci)
{
    Settings s(ci->mac);
    std::string pathAndFile = s.getPath("FLOPPY_IMAGE", "");     // try to read the value

    if(!pathAndFile.empty()) {      // this specific device has file? store it also to slot #
        logFdd(LOG_DEBUG, "CoreFdd::loadLastImageIntoSlot - have stored image for this specific device, saving to slot %d -> %s", ci->floppySlotIndex, pathAndFile.c_str());
        imageSilo->saveImageFilepathToSlot(ci->floppySlotIndex, pathAndFile.c_str());
    } else {                        // no image file for this specific device? try to get it from the slot #
        pathAndFile = imageSilo->getImageFilePathFromSlotNo(ci->floppySlotIndex);

        if(!pathAndFile.empty()) {  // slot # had an image stored? store it for this specific device
            logFdd(LOG_DEBUG, "CoreFdd::loadLastImageIntoSlot - no stored image for this specific device, but slot %d had image, so storing for specific device -> %s", ci->floppySlotIndex, pathAndFile.c_str());
            s.setPath("FLOPPY_IMAGE", pathAndFile);         // shown in the web UI
        } else {
            logFdd(LOG_DEBUG, "CoreFdd::loadLastImageIntoSlot - no stored image for this specific device and no stored image for slot %d, so no image to load", ci->floppySlotIndex);
        }
    }

    imageSilo->loadImageToSlot(ci->floppySlotIndex, pathAndFile.c_str());
}

void CoreFdd::handleFwVersion_franz(ClientInfo* ci)
{
    // ciFdd->setFDDconfig(setFloppyConfig, &floppyConfig, setDiskChanged, diskChanged);
    bool macChanged = ciFdd->getFWversionFdd(ci);

    if(macChanged) {
        loadLastImageIntoSlot(ci);
    }
}

void CoreFdd::handleSendTrack(ClientInfo* ci)
{
    uint8_t inBuf[BFR_SIZE];

    int readCnt = ciFdd->readRestOfData(ci, inBuf, BFR_SIZE);

    if(readCnt < 2) {
        logFdd(LOG_ERROR, "handleSendTrack() -- not enough data received: %d", readCnt);
        return;
    }

    int side = inBuf[0];                      // now read the current floppy position
    int track = inBuf[1];

    int tr, si, spt;
    imageSilo->getParams(ci->floppySlotIndex, tr, si, spt);      // read the floppy image params

    uint8_t *encodedTrack;
    int countInTrack = 0;

    if(side < 0 || side > 1 || track < 0 || track >= tr) {      // side / track out of range? use empty track
        logFdd(LOG_ERROR, "handleSendTrack() -- side / track out of range, returning empty track. Franz wants: [track %d, side %d], but current image has %d tracks, %d sides, %d sectors/track", track, side, tr, si, spt);

        encodedTrack = imageSilo->getEmptyTrack();
        countInTrack = MFM_STREAM_SIZE;
    } else {                                                    // side + track within range? use encoded track
        logFdd(LOG_DEBUG, "handleSendTrack() -- Franz wants: [track %d, side %d]", track, side);

        encodedTrack = imageSilo->getEncodedTrack(ci->floppySlotIndex, track, side, countInTrack);
        countInTrack = MIN(countInTrack, MFM_STREAM_SIZE);
    }

    ciFdd->fdd_sendTrackToChip(ci->fdClient, countInTrack, encodedTrack);
}

void CoreFdd::handleSendImageToClient(ClientInfo* client)
{
    std::string fileName = imageSilo->getFileName(client->floppySlotIndex);   // get the filename
    uint32_t imageGen = imageSilo->getImageGen(client->floppySlotIndex);       // which image generation we're sending

    int imgTracks, imgSides, imgSectorsPerTrack;
    imageSilo->getParams(client->floppySlotIndex, imgTracks, imgSides, imgSectorsPerTrack);      // read the floppy image params

    uint8_t *encodedTrack;
    int countInTrack = 0;

    // sending started, send imagTracks, imgSides, spt
    ciFdd->fdd_sendImageParamsToChip(client->fdClient, false, imgTracks, imgSides, imgSectorsPerTrack, fileName);

    // send all the tracks from all the sides
    for(int side=0; side<imgSides; side++) {
        for(int track=0; track<imgTracks; track++) {
            // logFdd(LOG_DEBUG, "handleSendImage -- sending track %d, side %d, countInTrack: %d", track, side, countInTrack);
            encodedTrack = imageSilo->getEncodedTrack(client->floppySlotIndex, track, side, countInTrack);
            countInTrack = MIN(countInTrack, MFM_STREAM_SIZE);
            ciFdd->fdd_sendTrackToChip(client->fdClient, countInTrack, encodedTrack);
        }
    }

    // sending finished, send imagTracks, imgSides, spt, this will also do the disk change
    ciFdd->fdd_sendImageParamsToChip(client->fdClient, true, imgTracks, imgSides, imgSectorsPerTrack, fileName);

    // device switches to the sent image now - sectors it writes from now on belong to this image, the previous one can be released
    imageSilo->setDeviceGen(client->floppySlotIndex, imageGen);

    if(browse[client->index].wanted) {      // device browses the images? the current image (and maybe the folder) changed
        sendImageList(client);
    }
}

void CoreFdd::imageBrowseReset(int clientIndex)
{
    if(clientIndex < 0 || clientIndex >= MAX_CLIENTS) {
        return;
    }

    ImageBrowse& b = browse[clientIndex];
    b.wanted = false;
    b.start = IMAGE_LIST_NONE;
    b.count = IMAGE_LIST_PAGE_MAX;
    b.list.setFolder("");               // forget the names of the previous client
}

void CoreFdd::updateImageListFolder(ClientInfo* ci, std::string& fileName)
{
    std::string pathAndFile = imageSilo->getBrowsePath(ci->floppySlotIndex);   // "" for no image / special image
    std::string folder;
    fileName.clear();

    if(!pathAndFile.empty()) {
        Utils::splitFilenameFromPath(pathAndFile, folder, fileName);

        if(folder.empty() && pathAndFile[0] == HOSTPATH_SEPAR_CHAR) {   // image in the root dir
            folder = HOSTPATH_SEPAR_STRING;
        }
    } else if(imageSilo->isSlotEmpty(ci->floppySlotIndex)) {   // no image in the drive? browse the downloaded images
        folder = Utils::dotEnvValue("FLOPPY_IMAGES_DIR", "", false);

        while(folder.size() > 1 && folder[folder.size() - 1] == HOSTPATH_SEPAR_CHAR) {    // "/x/" -> "/x", as in the image paths
            folder.resize(folder.size() - 1);
        }
    }

    browse[ci->index].list.setFolder(folder);
}

void CoreFdd::sendImageList(ClientInfo* ci)
{
    ImageBrowse& b = browse[ci->index];

    std::string fileName;
    updateImageListFolder(ci, fileName);

    uint8_t bfr[IMAGE_LIST_PAGE_MAX_SIZE];
    uint32_t current = fileName.empty() ? IMAGE_LIST_NONE : b.list.indexOf(fileName);
    uint32_t len = b.list.storePage(bfr, b.start, b.count, current);

    logFdd(LOG_DEBUG, "sendImageList -- client %d: gen %u, total %u, current %d, start %u, count %u", ci->index,
        Utils::getDword(bfr), Utils::getDword(bfr + 4), (int) Utils::getDword(bfr + 8), Utils::getDword(bfr + 12), Utils::getWord(bfr + 16));

    ciFdd->sendHeaderAndDataToChip(ci->fdClient, ATN_GET_IMAGE_LIST, bfr, len);
}

void CoreFdd::handleGetImageList(ClientInfo* ci)
{
    uint8_t inBuf[BFR_SIZE];
    int readCnt = ciFdd->readRestOfData(ci, inBuf, BFR_SIZE);

    if(readCnt < 6) {
        logFdd(LOG_ERROR, "handleGetImageList() -- not enough data received: %d", readCnt);
        return;
    }

    ImageBrowse& b = browse[ci->index];
    b.wanted = true;
    b.start = Utils::getDword(inBuf);
    b.count = MIN(Utils::getWord(inBuf + 4), IMAGE_LIST_PAGE_MAX);

    std::string fileName;
    updateImageListFolder(ci, fileName);
    b.list.refresh();                       // a change not seen by checkImageLists() yet

    sendImageList(ci);
}

void CoreFdd::handleInsertImage(ClientInfo* ci)
{
    uint8_t inBuf[INSERT_BFR_SIZE + 1];
    int readCnt = ciFdd->readRestOfData(ci, inBuf, INSERT_BFR_SIZE);

    uint8_t status = INSERT_IMAGE_FAILED;
    std::string name;

    if(readCnt >= 9 && inBuf[8] > 0 && (9 + inBuf[8]) <= readCnt) {
        name.assign((const char*) inBuf + 9, inBuf[8]);
    }

    ImageBrowse& b = browse[ci->index];
    std::string fileName;
    updateImageListFolder(ci, fileName);

    if(name.empty() || name.find(HOSTPATH_SEPAR_CHAR) != std::string::npos) {     // no name, or not just a name? bad request
        logFdd(LOG_ERROR, "handleInsertImage() -- bad request, %d bytes received", readCnt);
    } else if(b.list.getFolder().empty()) {
        logFdd(LOG_WARNING, "handleInsertImage() -- %s: current image can't be browsed", name.c_str());
    } else {
        b.list.refresh();                   // the device might have seen the list before a change

        if(b.list.indexOf(name) == IMAGE_LIST_NONE) {
            logFdd(LOG_WARNING, "handleInsertImage() -- %s: not in %s (device gen %u, index %u, list gen %u)", name.c_str(),
                b.list.getFolder().c_str(), Utils::getDword(inBuf), Utils::getDword(inBuf + 4), b.list.getGen());
            status = INSERT_IMAGE_NOT_FOUND;
        } else {
            std::string pathAndFile = Utils::mergeHostPaths2(b.list.getFolder(), name);
            logFdd(LOG_DEBUG, "handleInsertImage() -- inserting %s into slot %d", pathAndFile.c_str(), ci->floppySlotIndex);
            imageSilo->loadImageToSlot(ci->floppySlotIndex, pathAndFile.c_str());     // also stores it for the web UI
            status = INSERT_IMAGE_OK;
        }
    }

    ciFdd->sendHeaderAndDataToChip(ci->fdClient, ATN_INSERT_IMAGE, &status, 1);

    if(status == INSERT_IMAGE_NOT_FOUND && b.wanted) {  // device has an old list? send the current one
        sendImageList(ci);
    }
}

void CoreFdd::checkImageLists(void)
{
    for(int i=0; i<MAX_CLIENTS; i++) {
        ClientInfo* ci = ciFdd->clientGetByIndex(i);

        if(ci->fdClient == FD_EMPTY || !browse[i].wanted) {     // no client, or it doesn't browse? skip it
            continue;
        }

        if(browse[i].list.refresh()) {
            sendImageList(ci);
        }
    }
}

void CoreFdd::handleSectorWritten(ClientInfo* ci)
{
    int side, track, sector, byteCount;
    uint8_t *writtenSector = ciFdd->fdd_sectorWritten(ci, side, track, sector, byteCount); // get side + track + sector number, byte count, and pointer to buffer where the written data is

    if(byteCount <= 0) {        // no sector data received (maybe not even side / track / sector)? don't store anything
        logFdd(LOG_WARNING, "handleSectorWritten -- no sector data received, ignoring");
        return;
    }

    // Debug::outBfr(LOGFILE_FDD, writtenSector, byteCount);

    logFdd(LOG_DEBUG, "handleSectorWritten -- track %d, side %d, sector %d", track, side, sector);
    floppyEncoder_decodeMfmWrittenSector(ci->floppySlotIndex, track, side, sector, writtenSector, byteCount); // let floppy encoder handle decoding, reencoding, saving
}

bool CoreFdd::parseMac(const std::string& macStr, uint8_t* mac)
{
    if (macStr.size() != 12)
    {
        logFdd(LOG_WARNING, "MAC must be 12 hex characters, got: '%s'", macStr.c_str());
        return false;
    }

    for(size_t i = 0; i < 12; i++)
    {
        if(!isxdigit((unsigned char) macStr[i])) {      // not a hex digit? whole MAC invalid (don't leave zeros that could match some client)
            logFdd(LOG_WARNING, "MAC must be 12 hex characters, got: '%s'", macStr.c_str());
            return false;
        }
    }

    for(size_t i = 0; i < 6; i++)
    {
        char hex[3] = {macStr[2 * i], macStr[2 * i + 1], 0};
        mac[i] = (uint8_t) strtoul(hex, NULL, 16);
    }

    return true;
}

void CoreFdd::handleFddAction(int fddAction, const std::string& fddMac)
{
    uint8_t mac[6];
    memset(mac, 0, 6);
    if(!parseMac(fddMac, mac)) {        // invalid MAC? don't look up a client by all-zero MAC
        logFdd(LOG_WARNING, "Failed to handle fdd action, invalid mac '%s'", fddMac.c_str());
        return;
    }

    ClientInfo* client = ciFdd->clientsGetOneByMac(mac);

    if(!client) {
        logFdd(LOG_WARNING, "Failed to handle fdd action, because client with mac %s not found", fddMac.c_str());
        return;
    }

    if(fddAction == FDD_ACTION_EJECT) {
        logFdd(LOG_DEBUG, "ejecting slot %d", client->floppySlotIndex);
        imageSilo->remove(client->floppySlotIndex);
        return;
    }

    if(fddAction == FDD_ACTION_INSERT) {
        Settings s(client->mac);
        std::string pathAndFile = s.getPath("FLOPPY_IMAGE", "");   // try to read the value
        logFdd(LOG_DEBUG, "inserting %s into slot %d", pathAndFile.c_str(), client->floppySlotIndex);
        imageSilo->loadImageToSlot(client->floppySlotIndex, pathAndFile.c_str());
        return;
    }
}
