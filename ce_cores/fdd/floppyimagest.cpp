// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <string.h>
#include "floppyimagest.h"
#include "../misc/debug.h"

FloppyImageSt::~FloppyImageSt()
{
    clear();                            // save if needed (save() still dispatches to this class here), free memory
}

bool FloppyImageSt::open(const char *fileName)
{
    loadedFlag = false;

    if(!FloppyImage::open(fileName))
        return false;

    bool res = loadImageIntoMemory();   // load the whole image in memory to avoid later disk access
    close();                            // close the file, all the needed data is now in memory

    if(!res) {                          // if failed to load image, quit and fail
        clear();
        return false;
    }

    if(!calcParams()) {                 // couldn't figure out tracks / sides / sectors from image size? fail, don't use garbage geometry
        logFdd(LOG_ERROR, "ST Image %s has unsupported size %d bytes, not loading", fileName, image.size);
        clear();
        return false;
    }

    loadedFlag = true;

    logFdd(LOG_DEBUG, "ST Image opened: %s", fileName);
    logFdd(LOG_DEBUG, "ST Image params - %d tracks, %d sides, %d sectors per track", params.tracksNo, params.sidesNo, params.sectorsPerTrack);

    return true;
}

bool FloppyImageSt::calcParams(void)
{
    params.isInit = false;

    int cnt = image.size / 512;         // convert count of bytes to count of sectors

    if(cnt == 1440) {                   // if the size seems to be standard 80/2/9
        params.tracksNo         = 80;
        params.sidesNo          = 2;
        params.sectorsPerTrack  = 9;

        params.isInit           = true;
        return true;
    }

    if(cnt < 750) {                     // too little sectors?
        params.sidesNo = 1;             // single sided
    } else {
        params.sidesNo = 2;             // double sided
    }
    int sides = params.sidesNo;

    // now try to guess the right combination of track count and sectors per track count
    // note: 77,2,12 and 84,2,11 have the same sector count, also 78,2,14 and 84,2,13
    for(int track=75; track<86; track++) {
        for(int spt=5; spt<16; spt++) {
            int sectors = sides * track * spt;

            if(sectors == cnt) {
                params.tracksNo         = track;
                params.sectorsPerTrack  = spt;

                params.isInit           = true;
                return true;
            }
        }
    }

    logFdd(LOG_ERROR, "Couldn't guess the floppy params :(");
    return false;
}

bool FloppyImageSt::save(void)
{
    sectorsWritten = 0;                 // clear unwritten sectors counter

    if(!isLoaded()) {                   // nothing in memory? fail
        return false;
    }

    // write to temp file and rename it over the original - a crash during save can't destroy the image
    return saveFileAtomically(currentFileName.c_str(), image.data, image.size);
}
