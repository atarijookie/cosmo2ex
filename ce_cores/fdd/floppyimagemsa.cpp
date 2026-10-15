// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdlib.h>
#include <string.h>
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "floppyimagemsa.h"
#include "msa.h"
#include "mfmcachedimage.h"      // MAX_SECTORS_PER_TRACK_IN_STREAM

FloppyImageMsa::~FloppyImageMsa()
{
    clear();                            // save if needed (save() still dispatches to this class here), free memory
}

bool FloppyImageMsa::open(const char *fileName)
{
    loadedFlag = false;

    if(!FloppyImage::open(fileName))
        return false;

    // Read header - 5 big endian words
    uint16_t hdr[5];
    if(fread(hdr, 2, 5, fajl) != 5) {   // file too short for header?
        logFdd(LOG_ERROR, "MSA Image %s - file too short for MSA header", fileName);
        close();                // close file
        clear();                // clear memory
        return false;
    }

    uint16_t id         = Utils::SWAPWORD2(hdr[0]);
    uint16_t spt        = Utils::SWAPWORD2(hdr[1]);
    uint16_t sides      = Utils::SWAPWORD2(hdr[2]);
    uint16_t trackStart = Utils::SWAPWORD2(hdr[3]);
    uint16_t trackEnd   = Utils::SWAPWORD2(hdr[4]);

    if(id != 0x0e0f) {          // MSA ID mismatch?
        logFdd(LOG_ERROR, "MSA Image %s - bad MSA ID %04x", fileName, id);
        close();                // close file
        clear();                // clear memory
        return false;
    }

    if(spt < 1 || spt > 64 || sides > 1 || trackStart > trackEnd || trackEnd > 255) {  // header doesn't make sense? (same checks as in MSA_UnCompress)
        logFdd(LOG_ERROR, "MSA Image %s - invalid header: sectors/track: %d, sides: %d, tracks: %d..%d", fileName, spt, sides + 1, trackStart, trackEnd);
        close();
        clear();
        return false;
    }

    if(spt > MAX_SECTORS_PER_TRACK_IN_STREAM) {     // more sectors than fit in the encoded track's stream table? can't be streamed
        logFdd(LOG_ERROR, "MSA Image %s - has %d sectors per track, max supported is %d (HD images are not supported)", fileName, spt, MAX_SECTORS_PER_TRACK_IN_STREAM);
        close();
        clear();
        return false;
    }

    firstStoredTrack        = trackStart;       // tracks 0 .. trackStart-1 are not in the file - they'll be empty in memory
    params.tracksNo         = trackEnd + 1;     // image in memory always starts at track 0, so track numbers match what ST asks for
    params.sidesNo          = sides + 1;
    params.sectorsPerTrack  = spt;
    params.isInit           = true;

    bool res = loadImageIntoMemory();   // load the whole image in memory to avoid later disk access
    close();                            // close file - no need for it to be open

    if(!res) {              // load failed?
        clear();            // clear memory
        return false;
    }

    loadedFlag = true;

    logFdd(LOG_DEBUG, "MSA Image opened: %s", fileName);
    logFdd(LOG_DEBUG, "MSA Image params - %d tracks, %d sides, %d sectors per track", params.tracksNo, params.sidesNo, params.sectorsPerTrack);

    return true;
}

bool FloppyImageMsa::loadImageIntoMemory(void)
{
    if(!FloppyImage::loadImageIntoMemory()) {
        return false;
    }

    long imageSize = 0;
    uint8_t *pDiskBuffer = MSA_UnCompress(image.data, image.size, &imageSize);    // pass file size, so corrupt / truncated file can't make it read beyond

    free(image.data);                           // free the memory which was used for file reading

    image.data = pDiskBuffer;                   // store pointer to buffer with decompressed image and size
    image.size = imageSize;

    if(image.data == NULL) {                    // if the MSA_UnCompress failed, error
        return false;
    }

    if(firstStoredTrack > 0) {                  // file starts at later track? put stored tracks at their real position, tracks before are empty
        int bytesPerTrack   = params.sidesNo * params.sectorsPerTrack * 512;   // both sides
        int skippedBytes    = firstStoredTrack * bytesPerTrack;
        int fullSize        = skippedBytes + image.size;

        uint8_t *full = (uint8_t *) malloc(fullSize);
        if(full == NULL) {
            return false;
        }

        memset(full, 0, skippedBytes);
        memcpy(full + skippedBytes, image.data, image.size);

        free(image.data);
        image.data = full;
        image.size = fullSize;
    }

    return true;
}

bool FloppyImageMsa::save(void)
{
    sectorsWritten = 0;                         // clear unwritten sectors counter

    if(!isLoaded()) {                           // nothing in memory? fail
        return false;
    }

    // Tracks before firstStoredTrack weren't in the original file. Keep it that way, unless the ST wrote something there
    // (e.g. formatted track 0) - then save from track 0, otherwise that data would be lost.
    int startTrack = firstStoredTrack;
    int leadingBytes = firstStoredTrack * params.sidesNo * params.sectorsPerTrack * 512;

    for(int i=0; i<leadingBytes; i++) {
        if(image.data[i] != 0) {
            startTrack = 0;
            break;
        }
    }

    // save with the geometry from the original MSA header (what we loaded with), not with what the boot sector claims
    return MSA_WriteDisk(currentFileName.c_str(), image.data, image.size, params.tracksNo, params.sidesNo, params.sectorsPerTrack, startTrack);
}
