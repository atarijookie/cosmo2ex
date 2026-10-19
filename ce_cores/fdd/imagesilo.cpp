// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <signal.h>

#include "../misc/utils.h"
#include "../misc/debug.h"
#include "../misc/settings.h"
#include "imagesilo.h"
#include "floppyencoder.h"

extern pthread_mutex_t floppyEncoderMutex;
extern pthread_cond_t  floppyEncoderShouldWork;

extern SiloSlot slots[SLOT_COUNT];
//-------------------------------

ImageSilo::ImageSilo(ChipInterface *chipInterface)
{
    this->chipInterface = chipInterface;

    //-----------
    // create empty track, which can be used when the equested track is out of range (or image doesn't exist)
    emptyTrack = new uint8_t[MFM_STREAM_SIZE];
    memset(emptyTrack, 0, MFM_STREAM_SIZE);

    trackCopy = new uint8_t[MFM_STREAM_SIZE];  // encoded track is copied here before sending, see getEncodedTrack()
    memset(trackCopy, 0, MFM_STREAM_SIZE);

    uint8_t *p = emptyTrack;
    for(int i=0; i<(MFM_STREAM_SIZE/3); i++) {        // fill the empty track with 0x4e sync bytes
        *p++ = 0xa9;
        *p++ = 0x6a;
        *p++ = 0x96;
    }

    //-----------
    // init slots
    for(int i=0; i<SLOT_COUNT; i++) {
        clearSlot(i);
    }

    loadSettings();
}

ImageSilo::~ImageSilo()
{
    delete []emptyTrack;
    delete []trackCopy;
}

uint8_t *ImageSilo::getEmptyTrack(void)
{
    return emptyTrack;
}

void ImageSilo::loadSettings(void)
{
    for(int slot=0; slot<SLOT_COUNT; slot++) {
        std::string pathAndFile = getImageFilePathFromSlotNo(slot);
        loadImageToSlot(slot, pathAndFile.c_str());
    }
}

std::string ImageSilo::getImageFilePathFromSlotNo(int slotNo)
{
    std::string res;

    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        logFdd(LOG_WARNING, "ImageSilo::getImageFilePathFromSlotNo -- slotNo: %d invalid", slotNo);
        return res;
    }

    Settings s;

    char key[32];
    snprintf(key, sizeof(key), "FLOPPY_IMAGE_%d", slotNo);      // create settings key
    res = s.getPath(key, "");         // get the value

    return res;
}

void ImageSilo::saveImageFilepathToSlot(int slotNo, const char* pathAndFile)
{
    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        logFdd(LOG_WARNING, "ImageSilo::saveImageFilepathToSlot -- slotNo: %d invalid", slotNo);
        return;
    }

    Settings s;

    char key[32];
    snprintf(key, sizeof(key), "FLOPPY_IMAGE_%d", slotNo);      // create settings key

    s.setPath(key, pathAndFile);       // store the value
    logFdd(LOG_DEBUG, "ImageSilo::saveImageFilepathToSlot -- slotNo: %d, file: %s", slotNo, pathAndFile);
}

void ImageSilo::saveImageFilepathToClient(int slotNo, const char* pathAndFile)
{
    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        logFdd(LOG_WARNING, "ImageSilo::saveImageFilepathToClient -- slotNo: %d invalid", slotNo);
        return;
    }

    ClientInfo* client = chipInterface->clientsGetOneByFloppySlot(slotNo);
    if(!client) {
        logFdd(LOG_WARNING, "ImageSilo::saveImageFilepathToClient -- slotNo: %d - no client found", slotNo);
        return;
    }

    Settings s;
    s.setPrefix(client->mac, 6);                // mac as prefix to settings
    s.setPath("FLOPPY_IMAGE", pathAndFile);     // store the value - shown in the web UI

    logFdd(LOG_DEBUG, "ImageSilo::saveImageFilepathToClient -- slotNo: %d, file: %s", slotNo, pathAndFile);
}

void ImageSilo::loadImageToSlot(int slotNo, const char* pPathAndFile)
{
    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        logFdd(LOG_WARNING, "ImageSilo::loadImageToSlot -- slotNo: %d invalid", slotNo);
        return;
    }

    std::string pathAndFile = pPathAndFile;

    if(pathAndFile.empty()) {                   // nothing stored? skip it
        logFdd(LOG_DEBUG, "ImageSilo::loadImageToSlot -- slotNo: %d, pathAndFile: %s - empty!", slotNo, pathAndFile.c_str());
        // TODO: add() empty image
        return;
    }

    std::string path, file;
    Utils::splitFilenameFromPath(pathAndFile, path, file);
    logFdd(LOG_DEBUG, "ImageSilo::loadImageToSlot -- slotNo: %d, pathAndFile: %s, file: %s", slotNo, pathAndFile.c_str(), file.c_str());
    add(slotNo, file, pathAndFile);        // add this image
}

void ImageSilo::add(int positionIndex, std::string &filename, std::string &hostPath)
{
    if(positionIndex < 0 || positionIndex >= SLOT_COUNT) {
        return;
    }

    saveImageFilepathToClient(positionIndex, hostPath.c_str()); // for this client's mac store image file path

    logFdd(LOG_DEBUG, "ImageSilo::add() -- positionIndex: %d, filename: %s, hostPath: %s", positionIndex, filename.c_str(), hostPath.c_str());

    // store the info about slot
    slots[positionIndex].imageFile = filename;  // just file name: bla.st
    slots[positionIndex].hostPath = hostPath;   // where the file is stored on disk

    // create and add floppy encode request
    floppyEncoder_addEncodeWholeImageRequest(positionIndex, hostPath.c_str());
}

void ImageSilo::remove(int index)                   // remove image at specified slot
{
    if(index < 0 || index >= SLOT_COUNT) {
        return;
    }

    logFdd(LOG_DEBUG, "ImageSilo::remove() -- ejecting slot %d (%s)", index, slots[index].imageFile.c_str());

    clearSlot(index);

    // forget the image in settings - for the device in this slot and for the slot itself, so it won't come back on reconnect
    saveImageFilepathToClient(index, "");
    saveImageFilepathToSlot(index, "");

    // unload the image in encoder thread (saves unsaved changes first), it will then send empty slot to device;
    // this also clears the remembered file name, so inserting the same image again works
    floppyEncoder_addEncodeWholeImageRequest(index, "");
}

void ImageSilo::clearSlot(int index)
{
    slots[index].imageFile.clear();
    slots[index].hostPath.clear();
}

uint32_t ImageSilo::getImageGen(int slotNo)
{
    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        return 0;
    }

    pthread_mutex_lock(&floppyEncoderMutex);
    uint32_t gen = slots[slotNo].imageGen;
    pthread_mutex_unlock(&floppyEncoderMutex);

    return gen;
}

void ImageSilo::setDeviceGen(int slotNo, uint32_t gen)
{
    if(slotNo < 0 || slotNo >= SLOT_COUNT) {
        return;
    }

    pthread_mutex_lock(&floppyEncoderMutex);
    slots[slotNo].deviceGen = gen;          // from now on, sectors written by device belong to this image generation
    pthread_mutex_unlock(&floppyEncoderMutex);

    logFdd(LOG_DEBUG, "ImageSilo::setDeviceGen -- slot %d: device now has image generation %u", slotNo, gen);
}

uint8_t *ImageSilo::getEncodedTrack(int floppySlotIndex, int track, int side, int &bytesInBuffer)
{
    // Returns a COPY of the encoded track (in trackCopy, valid until next call), never a pointer into the encoder's cache -
    // the encoder thread could start re-encoding (memset + rewrite) that track while we're still sending it.
    // All callers run in the single FDD thread and send the data right away, so one copy buffer is enough.

    bytesInBuffer = MFM_STREAM_SIZE;        // count for emptyTrack, used on every early return below

    if(floppySlotIndex < 0 || floppySlotIndex >= SLOT_COUNT) {
        return emptyTrack;
    }

    MfmCachedImage *encImage = &slots[floppySlotIndex].encImage;

    if(encImage->copyEncodedTrackIfReady(track, side, trackCopy, bytesInBuffer)) {   // ready? got the copy
        return trackCopy;
    }

    floppyEncoder_addReencodeTrackRequest(floppySlotIndex, track, side);    // not ready - ask for (re)encoding

    // wait short while to see if the track gets encoded
    Timeout timeout(500);

    while(!timeout.expired()) {                 // still should wait?
        if(encImage->copyEncodedTrackIfReady(track, side, trackCopy, bytesInBuffer)) {   // ready now? got the copy
            return trackCopy;
        }

        Utils::sleepMs(1);  // don't spin - give the CPU to the encoder thread, which is the one making the track ready
    }

    bytesInBuffer = MFM_STREAM_SIZE;            // not ready in time - return empty track
    return emptyTrack;
}

bool ImageSilo::getParams(int floppySlotIndex, int &tracks, int &sides, int &sectorsPerTrack)
{
    if(floppySlotIndex < 0 || floppySlotIndex >= SLOT_COUNT) {
        tracks = 0;
        sides = 0;
        sectorsPerTrack = 0;
        return false;
    }

    return slots[floppySlotIndex].encImage.getParams(tracks, sides, sectorsPerTrack);
}

std::string ImageSilo::getFileName(int floppySlotIndex)
{
    if(floppySlotIndex < 0 || floppySlotIndex >= SLOT_COUNT) {
        logFdd(LOG_DEBUG, "getFileName() -- floppySlotIndex out of bounds: %d, returning empty string", floppySlotIndex);
        std::string empty;
        return empty;
    }

    logFdd(LOG_DEBUG, "getFileName() -- floppySlotIndex: %d, returning: %s", floppySlotIndex, slots[floppySlotIndex].imageFile.c_str());
    return slots[floppySlotIndex].imageFile;
}
