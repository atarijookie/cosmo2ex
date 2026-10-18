// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#ifndef _IMAGESILO_H_
#define _IMAGESILO_H_

#include <pthread.h>
#include <stdint.h>
#include <string>
#include <queue>

#include "../chipinterface/chipinterface.h"
#include "mfmcachedimage.h"

#define SLOT_COUNT          MAX_CLIENTS

class ChipInterface;

typedef struct
{
    int slotNo;                     // slot number of this slot (used for debugging)

    std::string     imageFile;      // just file name:                     bla.st
    std::string     hostPath;       // for translated disk, host path:     /mnt/sda/gamez/bla.st
                                    // or where uploaded disk where is stored: /tmp/bla.st

    volatile bool   openRequested;  // set to true when doing open file request, set to false after successful open
    uint32_t        openRequestSeq; // incremented (under mutex) on every open request - counter, not time, so 2 requests in the same ms aren't mixed up
    uint32_t        openHandledSeq; // openRequestSeq value the encoder saw when it started opening; equal = no newer request
    std::string     imageFileName;  // file name of image to open next
    FloppyImage     *image;         // this holds object with the loaded floppy image (in normal data form)

    // While a new image is being encoded and sent, the device (and ST) still has the previous one - keep it, so sectors
    // written to it in that time are stored in the right image. Each loaded image gets a generation number; written sectors
    // are tagged with the generation the device had at that time. Generation 0 = unknown (device was never sent an image).
    FloppyImage     *oldImage;      // previous image, kept until the device has received the current one (encoder thread only)
    uint32_t        imageGen;       // generation of 'image', incremented on every load / eject (written by encoder thread under mutex)
    uint32_t        oldImageGen;    // generation of 'oldImage' (encoder thread only)
    uint32_t        deviceGen;      // generation of the image the device has (written by FDD thread under mutex, after sending whole image)

    MfmCachedImage  encImage;       // this holds the MFM encoded image ready to be streamed
} SiloSlot;

class ImageSilo
{
public:
    ImageSilo(ChipInterface *chipInterface);
    ~ImageSilo();

    static void run(void);
    static void stop(void);

    void loadSettings(void);

    uint8_t *getEncodedTrack(int floppySlotIndex, int track, int side, int &bytesInBuffer);
    bool getParams(int floppySlotIndex, int &tracks, int &sides, int &sectorsPerTrack);
    std::string getFileName(int floppySlotIndex);
    uint8_t *getEmptyTrack(void);

    void add(int positionIndex, std::string &filename, std::string &hostPath);
    void remove(int index);

    std::string getImageFilePathFromSlotNo(int slotNo);
    void saveImageFilepathToSlot(int slotNo, const char* pathAndFile);
    void saveImageFilepathToClient(int slotNo, const char* pathAndFile);
    void loadImageToSlot(int slotNo, const char* pPathAndFile);

    uint32_t getImageGen(int slotNo);                   // generation of image currently in slot - read before sending whole image to device
    void setDeviceGen(int slotNo, uint32_t gen);        // call after whole image was sent to device - device now has this generation

private:
    ChipInterface *chipInterface;

    void clearSlot(int index);

    uint8_t *emptyTrack;
    uint8_t *trackCopy;                 // copy of encoded track returned by getEncodedTrack()
};

#endif
