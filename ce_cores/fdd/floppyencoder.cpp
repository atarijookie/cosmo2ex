// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <stdlib.h>
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
#include "floppyimagemsa.h"
#include "floppyimagest.h"
#include "imagesilo.h"
#include "floppyencoder.h"

pthread_mutex_t floppyEncoderMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  floppyEncoderShouldWork = PTHREAD_COND_INITIALIZER;

//volatile bool floppyEncodingRunning = false;
volatile bool shouldStop;

//-------------------------------
// silo slots are global objects, as FloppyThread needs to stream from them and
// encoder thread needs to check all of them for tracks that need (re)encoding.
// Access is protected by floppyEncoderMutex.
// Individual tracks in slot can be streamed without using mutex when: slot.encImage.tracks[index].isReady
// (that means we're not encoding that track at that moment).

SiloSlot slots[SLOT_COUNT];
volatile uint8_t slotSendToDevice[SLOT_COUNT];

WrittenMfmSector writtenSectors[WRITTENMFMSECTOR_COUNT];
//-------------------------------

void floppyEncoder_addEncodeWholeImageRequest(int slotNo, const char *imageFileName)
{
    pthread_mutex_lock(&floppyEncoderMutex);    		// lock the mutex

    SiloSlot *slot = &slots[slotNo];            		// get pointer to the right slot
    std::string inImageFileName = imageFileName;

    if(inImageFileName != slot->imageFileName) {        // slot has different image? load
        logFdd(LOG_DEBUG, "floppyEncoder_addEncodeWholeImageRequest -- slotNo: %d, image: %s - image changed, loading", slotNo, imageFileName);

        slot->openRequested = true;
        slot->openRequestSeq++;                         // one more open request
        slot->imageFileName = inImageFileName;	        // store image file name

        pthread_cond_signal(&floppyEncoderShouldWork);  	// wake up encoder
    } else {        // image not changed, don't load
        logFdd(LOG_WARNING, "floppyEncoder_addEncodeWholeImageRequest -- slotNo: %d, image: %s - image not changed, ignoring", slotNo, imageFileName);
    }

    pthread_mutex_unlock(&floppyEncoderMutex);      	// unlock the mutex
}

void floppyEncoder_addReencodeTrackRequest(int floppySlotIndex, int track, int side)
{
    logFdd(LOG_DEBUG, "floppyEncoder_addReencodeTrackRequest - track: %d, side: %d", track, side);

    pthread_mutex_lock(&floppyEncoderMutex);        // lock the mutex

    SiloSlot *slot = &slots[floppySlotIndex];       // get pointer to the right slot
    slot->encImage.askToReencodeTrack(track, side); // this specific track needs to be reencoded

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

static int findEmptyWrittenSector(void)
{
    int i;
    for(i=0; i<WRITTENMFMSECTOR_COUNT; i++) {           // go through the written sector storage
        if(!writtenSectors[i].hasData) {                // this one doesn't have data? use it
            if(writtenSectors[i].data == NULL) {        // buffer not allocated? allocate!
                writtenSectors[i].data = (uint8_t *) malloc(WRITTENMFMSECTOR_SIZE);

                if(writtenSectors[i].data == NULL) {    // out of memory? can't store
                    return -1;
                }
            }

            memset(writtenSectors[i].data, 0, WRITTENMFMSECTOR_SIZE);
            return i;       // return index to this sector
        }
    }

    // if came here, no empty sector in storage found
    return -1;
}

static int findWrittenSectorForProcessing(int onlySlotNo = -1)   // onlySlotNo: -1 = any slot, otherwise only sectors written to that slot
{
    int i;
    for(i=0; i<WRITTENMFMSECTOR_COUNT; i++) {           // go through the written sector storage
        if(writtenSectors[i].hasData && (onlySlotNo == -1 || writtenSectors[i].slotNo == onlySlotNo)) {  // this one has data (for wanted slot)? use it
            return i;       // return index to this sector
        }
    }

    // if came here, no empty sector in storage found
    return -1;
}

static void freeWrittenSectorStorage(void)
{
    int i;
    for(i=0; i<WRITTENMFMSECTOR_COUNT; i++) {       // go through the written sector storage
        if(writtenSectors[i].data != NULL) {        // buffer allocated? free it
            free(writtenSectors[i].data);
        }
    }
}

void floppyEncoder_decodeMfmWrittenSector(int floppySlotIndex, int track, int side, int sector, uint8_t *data, uint32_t size)
{
    if(floppySlotIndex < 0 || floppySlotIndex >= SLOT_COUNT) {
        logFdd(LOG_ERROR, "floppyEncoder_decodeMfmWrittenSector - invalid slot %d, sector not stored", floppySlotIndex);
        return;
    }

    if(size > WRITTENMFMSECTOR_SIZE) {              // if data too big to fit, fail
        logFdd(LOG_ERROR, "floppyEncoder_decodeMfmWrittenSector - size: %d > %d !!! sector not stored", size, WRITTENMFMSECTOR_SIZE);
        return;
    }

    pthread_mutex_lock(&floppyEncoderMutex);        // lock the mutex

    int index = findEmptyWrittenSector();           // try to find where this new sector could be stored
    logFdd(LOG_DEBUG, "floppyEncoder_decodeMfmWrittenSector - track: %d, side: %d, sector: %d, size: %d -> writtenSector index: %d", track, side, sector, size, index);

    if(index != -1) {                               // if was able to find empty place for this sector, store it
        WrittenMfmSector *wrSector = &writtenSectors[index];    // get pointer to it and store data and params
        wrSector->slotNo = floppySlotIndex;
        wrSector->imageGen = slots[floppySlotIndex].deviceGen;  // the image the device has right now is the one being written to
        wrSector->track = track;
        wrSector->side = side;
        wrSector->sector = sector;

        memcpy(wrSector->data, data, size);         // copy in the data
        wrSector->size = size;

        wrSector->hasData = true;                   // mark as ready only after everything is stored
    } else {                                        // storage full - this write is lost, that's data loss on the floppy
        logFdd(LOG_ERROR, "floppyEncoder_decodeMfmWrittenSector - written sector storage full (%d), DROPPING write of track %d, side %d, sector %d", WRITTENMFMSECTOR_COUNT, track, side, sector);
    }

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

void floppyEncoder_stop(void)
{
    pthread_mutex_lock(&floppyEncoderMutex);        // lock the mutex

    shouldStop = true;

    pthread_cond_signal(&floppyEncoderShouldWork);  // wake up encoder
    pthread_mutex_unlock(&floppyEncoderMutex);      // unlock the mutex
}

FloppyImage *getImage(const char *fileName)
{
    FloppyImage *img = NULL;
    const char *ext = Utils::getExtension(fileName);    // find extensions

    if(ext == NULL) {                       // extension not found? fail
        return NULL;
    }

    if(strcasecmp(ext, "msa") == 0) {   // msa image?
        logFdd(LOG_DEBUG, "using MSA image on %s", fileName);
        img = new FloppyImageMsa();
    } else if(strcasecmp(ext, "st") == 0) {    // st image?
        logFdd(LOG_DEBUG, "using ST image on %s", fileName);
        img = new FloppyImageSt();
    } else {
        logFdd(LOG_DEBUG, "Image file %s type %s not supported", fileName, ext);
    }

    if(img) {
        img->open(fileName);            // open the new image
    }
    return img;                        // unknown extension?
}

// Take one written sector out of the storage, decode it and write it into the slot's image.
// onlySlotNo: -1 = any slot, otherwise only sectors written to that slot. Returns false if there was nothing to process.
static bool floppyEncoder_processOneWrittenSector(int onlySlotNo)
{
    // take one written sector out of the storage - copy it under mutex, so we never see a half-stored sector
    // (+16 spare zeroed bytes: the MFM decoder may look 1 byte past the end of the data)
    static uint8_t wsData[WRITTENMFMSECTOR_SIZE + 16];
    WrittenMfmSector ws;
    bool gotWritten = false;

    pthread_mutex_lock(&floppyEncoderMutex);

    int writtenIdx = findWrittenSectorForProcessing(onlySlotNo);  // check if something was written

    if(writtenIdx != -1) {
        ws = writtenSectors[writtenIdx];            // copy params
        memset(wsData, 0, sizeof(wsData));
        memcpy(wsData, writtenSectors[writtenIdx].data, ws.size);  // copy data
        ws.data = wsData;

        writtenSectors[writtenIdx].hasData = false; // storage slot is free again
        gotWritten = true;
    }

    pthread_mutex_unlock(&floppyEncoderMutex);

    if(!gotWritten) {                               // nothing written (for that slot)? done
        return false;
    }

    // something was written - decode it and write it to image
    SiloSlot *ss = &slots[ws.slotNo];           // get pointer to slot where the sector should be stored

    // find the image the device had when writing this: the current one, or the previous one kept during image switch.
    // Generation 0 = we never sent the device an image (e.g. after restart it still has the one it had before) - use current, as before.
    FloppyImage *target = NULL;
    bool isCurrent = false;

    if(ws.imageGen == ss->imageGen || ws.imageGen == 0) {
        target = ss->image;
        isCurrent = true;
    } else if(ws.imageGen == ss->oldImageGen) {
        target = ss->oldImage;
    }

    uint8_t sectorData[512];
    bool good;
    good = ss->encImage.decodeMfmBuffer(ws.data, ws.size, sectorData);   // decode written sector data

    logFdd(LOG_DEBUG, "Written sector at index: %d decoded, good: %d, image gen: %u (current: %u, old: %u), got target image: %d",
        writtenIdx, good, ws.imageGen, ss->imageGen, ss->oldImageGen, target != NULL);

    if(good && !target) {       // decoded fine, but the image it belongs to is gone (or slot empty)
        logFdd(LOG_WARNING, "Written sector track %d, side %d, sector %d of slot %d: image generation %u no longer loaded, dropping write",
            ws.track, ws.side, ws.sector, ws.slotNo, ws.imageGen);
    }

    if(good && target) {        // if decode was good and got the image it belongs to
        if(ss->encImage.lastBufferWasFormatTrack()) {   // if it was FORMAT TRACK
            logFdd(LOG_DEBUG, "Detected FORMAT TRACK");

            int tracks, sides, sectorsPerTrack;
            target->getParams(tracks, sides, sectorsPerTrack);   // get how many sectors one track has

            memset(sectorData, 0, 512);                 // clear data buffer
            for(int i=1; i<=sectorsPerTrack; i++) {     // for all sectors write empty data
                target->writeSector(ws.track, ws.side, i, sectorData);
            }
        } else {                                        // if it was WRITE SECTOR
            logFdd(LOG_DEBUG, "Detected WRITE SECTOR");

            target->writeSector(ws.track, ws.side, ws.sector, sectorData);
        }

        if(isCurrent) {                                 // only the current image is encoded / streamed, the old one is just kept for writes
            pthread_mutex_lock(&floppyEncoderMutex);    // same lock as floppyEncoder_addReencodeTrackRequest()
            ss->encImage.askToReencodeTrack(ws.track, ws.side);   // this specific track needs to be reencoded
            pthread_mutex_unlock(&floppyEncoderMutex);
        }
    }

    return true;
}

// clear openRequested if no new open request came since we started handling it (otherwise handle it again in the next round)
static void markOpenHandledIfNoNewRequest(SiloSlot *slot)
{
    pthread_mutex_lock(&floppyEncoderMutex);

    if(slot->openRequestSeq == slot->openHandledSeq) {
        slot->openRequested = false;
    }

    pthread_mutex_unlock(&floppyEncoderMutex);
}

static void floppyEncoder_handleLoadFiles(void)
{
    for(int i=0; i<SLOT_COUNT; i++) {
        SiloSlot *slot = &slots[i];					        // get one slot

        if(!slot->openRequested) {	                        // if image open not request, skip it
            continue;
        }

        // sectors the ST wrote may still wait in the written sector storage - write them into their images now,
        // before the images get shuffled below
        int flushed = 0;
        while(floppyEncoder_processOneWrittenSector(i)) {
            flushed++;
        }

        if(flushed > 0) {
            logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles -- slot %d: wrote %d pending sector(s) into old image before switching", i, flushed);
        }

        pthread_mutex_lock(&floppyEncoderMutex);            // lock the mutex

        slot->encImage.clearWholeCachedImage();             // clear remains of previous stream
        slot->openHandledSeq = slot->openRequestSeq;        // we're handling all requests up to this one now
        std::string imageFileName = slot->imageFileName;    // make a copy of the image file name, so we can unlock mutex but still work with the filename

        uint32_t deviceGen = slot->deviceGen;               // which image the device has now
        uint32_t prevImageGen = slot->imageGen;
        slot->imageGen++;                                   // whatever gets loaded below (or nothing) is a new generation
        if(slot->imageGen == 0) {                           // 0 is reserved for 'unknown', skip it on wrap
            slot->imageGen = 1;
        }

        pthread_mutex_unlock(&floppyEncoderMutex);	        // unlock the mutex - the open bellow might take long, but slot->image is not touched by any other thread than floppyEncoder, so don't leave it locked

        // The device keeps using the image it has until it receives the new one, so keep that image for its writes.
        // Delete (= save if needed) only the image the device doesn't have.
        if(slot->oldImage && deviceGen == slot->oldImageGen && deviceGen != prevImageGen) {
            // device still has the old image - the current one never reached it (switched again quickly), drop the current one
            logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles -- slot %d: device still has image gen %u, dropping never-sent image gen %u", i, deviceGen, prevImageGen);
            delete slot->image;                             // destructor saves unsaved changes
        } else {
            // device has (or will have had) the current image - it becomes the old one, the previous old one is deleted
            if(slot->oldImage) {
                logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles -- slot %d: deleting old image gen %u from memory", i, slot->oldImageGen);
                delete slot->oldImage;                      // destructor saves unsaved changes
            }

            slot->oldImage      = slot->image;
            slot->oldImageGen   = prevImageGen;
        }

        slot->image = NULL;

        if(imageFileName.empty()) {                         // empty file name = eject, nothing to load
            logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles -- slot %d ejected", i);
            slotSendToDevice[i] = true;                     // let the device know - empty slot is sent as image with 0 tracks

            markOpenHandledIfNoNewRequest(slot);
            continue;
        }

        // try to load image from disk
        slot->image = getImage(imageFileName.c_str());

        if(!slot->image || !slot->image->isLoaded()) { // not supported image format or failed to open file?
            logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles - failed to load image %s", imageFileName.c_str());

            if(slot->image) {                       // if got the object, but failed to open, destory object and set pointer to null
                delete slot->image;
                slot->image = NULL;
            }
        } else {                                    // image loaded? good
            logFdd(LOG_DEBUG, "floppyEncoder_handleLoadFiles - image %s loaded", imageFileName.c_str());
            slot->encImage.storeImageParams(slot->image, i);    // sets tracksToBeEncoded to all tracks count
        }

        // if no open request happened during the loading, clear the openRequested flag and we're done here
        markOpenHandledIfNoNewRequest(slot);
    }
}

static void saveImageIfIdle(FloppyImage *img, uint32_t now, int slotNo, const char *which)
{
    if(img == NULL) {                           // if image not present, skip it
        return;
    }

    if(!img->gotUnsavedChanges()) {             // if this image doesn't have unsaved changes, skip it
        return;
    }

    uint32_t timeSinceLastWrite = now - img->getLastWriteTime();

    if(timeSinceLastWrite < 5000) {             // if last write happened too recently, wait with the write
        return;
    }

    logFdd(LOG_DEBUG, "floppyEncoder_handleSaveFiles -- saving %s image of slot %d", which, slotNo);
    img->save();                                // save the changes
}

static void floppyEncoder_handleSaveFiles(void)
{
    uint32_t now = Utils::getCurrentMs();              // current time

    for(int i=0; i<SLOT_COUNT; i++) {
        saveImageIfIdle(slots[i].image,    now, i, "current");
        saveImageIfIdle(slots[i].oldImage, now, i, "old");     // device might be still writing to it (or never switched, e.g. disconnected)
    }
}

// Once the device has received the current image, nothing will write to the old image anymore - save and free it.
static void floppyEncoder_releaseOldImages(void)
{
    for(int i=0; i<SLOT_COUNT; i++) {
        SiloSlot *slot = &slots[i];

        if(slot->oldImage == NULL) {            // no old image kept? skip
            continue;
        }

        pthread_mutex_lock(&floppyEncoderMutex);
        bool deviceHasCurrent = (slot->deviceGen == slot->imageGen);
        pthread_mutex_unlock(&floppyEncoderMutex);

        if(!deviceHasCurrent) {                 // device still has the old image (new one not sent yet)? keep it
            continue;
        }

        while(floppyEncoder_processOneWrittenSector(i)) {   // writes still queued for the old image go in before it's saved
        }

        logFdd(LOG_DEBUG, "floppyEncoder_releaseOldImages -- slot %d: device has image gen %u, releasing old image gen %u", i, slot->imageGen, slot->oldImageGen);
        delete slot->oldImage;                  // destructor saves unsaved changes
        slot->oldImage = NULL;
    }
}

static void floppyEncoder_doBeforeTerminating(void)
{
    while(floppyEncoder_processOneWrittenSector(-1)) {     // write all still pending written sectors into images, so they get saved below
    }

    for(int i=0; i<SLOT_COUNT; i++) {
        if(slots[i].image) {                    // if image in this slot is present
            slots[i].image->clear();            // save image if some unwritten data needs to be saved, free memory
            delete slots[i].image;              // delete image
            slots[i].image = NULL;
        }

        if(slots[i].oldImage) {                 // previous image still kept for device writes? save and free it too
            delete slots[i].oldImage;           // destructor saves unsaved changes
            slots[i].oldImage = NULL;
        }
    }

    freeWrittenSectorStorage();                 // free the storage for written sectors

    logFdd(LOG_DEBUG, "Floppy encode thread terminated.");
}

SiloSlot *findSlotToEncode(void)
{
    // go through the slots, if one of them has something for encoding, return that
    for(int i=0; i<SLOT_COUNT; i++) {
        if(slots[i].image != NULL && slots[i].encImage.somethingToBeEncoded()) {   // only slots with a loaded image can be encoded
            return &slots[i];
        }
    }

    // nothing found for encoding
    return NULL;
}

void *floppyEncodeThreadCode(void *ptr)
{
    logFdd(LOG_DEBUG, "Floppy encode thread starting...");

    for(int i=0; i<SLOT_COUNT; i++) {
        slots[i].slotNo = i;        // store own slot # for debugging purposes - some functions here return pointers to slots, this will tell us the slot # in that case
    }

    // the following code should encode all the slots, but one track at the time, so when there are multiple not encoded images,
    // the one which is selected for streaming, will be encoded first, and then the rest. This should also act fine if you switch
    // current slot in the middle of encoding image (to prioritize the selected one)
    Timeout pauseTimer(50);                            // this will help to add pauses at least every 50 ms to allow other threads to do stuff

    while(true) {
        floppyEncoder_handleSaveFiles();            // save all those images which need to be saved
        floppyEncoder_handleLoadFiles();            // check if some file needs loading and then load it
        floppyEncoder_releaseOldImages();           // free previous images the device doesn't use anymore

        SiloSlot *slot = findSlotToEncode();        // check if there is something to encode
        int writtenIdx = findWrittenSectorForProcessing();  // check if something was written

        if(!slot && writtenIdx == -1) {              // nothing to encode and nothing to decode after write? sleep
            struct timespec max_wait = {0, 0};
            clock_gettime(CLOCK_REALTIME, &max_wait);
            max_wait.tv_sec += 2;                   // wake up in this interval to check for late saving

            pthread_mutex_lock(&floppyEncoderMutex);    // lock the mutex
            pthread_cond_timedwait(&floppyEncoderShouldWork, &floppyEncoderMutex, &max_wait);   // unlock, sleep; wake up, lock
            pthread_mutex_unlock(&floppyEncoderMutex);  // unlock the mutex

            pauseTimer.restart();                   // after sleep - work for some time without pause
        }

        if(shouldStop) {                            // if we should already stop
            floppyEncoder_doBeforeTerminating();
            return 0;
        }

        if(pauseTimer.expired()) {                  // if at least 50 ms passed since start or previous pause, add a small pause so other threads could do stuff
            Utils::sleepMs(5);
            pauseTimer.restart();
        }

        slot = findSlotToEncode();  // find some slot which needs encoding

        if(slot) {                  // if some slot needs encoding, do it, otherwise skip encoding
            //floppyEncodingRunning = true;

            int track, side;
            slot->encImage.findNotReadyTrackAndEncodeIt(slot->image, track, side);

            if(track != -1) {       // if something was encoded, dump it to log
                //logFdd(LOG_DEBUG, "Encoding of [slot: %d, track %d, side %d] of image %s done", slot->slotNo, track, side, slot->image->getFileName());
            }

            //floppyEncodingRunning = false;
        }

        floppyEncoder_processOneWrittenSector(-1);    // if something was written, decode it and write it to image
    }

    floppyEncoder_doBeforeTerminating();
    return 0;
}
