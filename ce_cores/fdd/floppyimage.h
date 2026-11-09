// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef FLOPPYIMAGE_H
#define FLOPPYIMAGE_H

#include <stdio.h>
#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <time.h>

#include "../misc/global.h"

class MfmTrackBuilder;

class FloppyImage
{
public:
    FloppyImage();
    virtual ~FloppyImage();

    bool isLoaded(void);
    const char *getFileName(void);
    bool gotUnsavedChanges(void);                   // return if we should call save before clear()
    bool fileChangedOnDisk(void);                   // something else replaced the file since we loaded or saved it
    uint32_t getLastWriteTime(void);                   // return time when last write happened

    virtual bool open(const char *fileName);        // open filename for reading
    virtual void close(void);                       // close file handle if open

    virtual bool save(void) = 0;                    // save data into currentFileName
    virtual void clear(void);                       // clear the data in memory
    virtual bool getParams(int &tracks, int &sides, int &sectorsPerTrack);

    virtual bool readSector(int track, int side, int sectorNo, uint8_t *buffer);
    virtual bool writeSector(int track, int side, int sectorNo, uint8_t *buffer);
    virtual bool readNotWriteSector(bool readNotWrite, int track, int side, int sectorNo, uint8_t *buffer);

    virtual bool isReadOnly(void) { return false; }     // true: the ST gets a write protected disk, writes are dropped

    // Images with their own track layout build the track here. false = the image is just sectors, use the standard layout.
    virtual bool buildTrack(MfmTrackBuilder &builder, int track, int side) { return false; }

    // write whole file so that a crash / power loss during save never leaves a half-written image:
    // write to temp file next to it, fsync, then rename over the original
    static bool saveFileAtomically(const char *path, const uint8_t *data, size_t size);

protected:
    virtual bool loadImageIntoMemory(void);
    void rememberFileState(void);                   // call after loading or saving the file

    struct {
        int tracksNo;
        int sidesNo;
        int sectorsPerTrack;
        bool isInit;
    } params;

    struct {
        int size;
        uint8_t *data;
    } image;

    FILE *fajl;

    struct {
        bool    known;
        time_t  mtime;
        off_t   size;
        ino_t   inode;
    } fileState;

    std::string currentFileName;
    bool loadedFlag;

    int  sectorsWritten;        // count of sectors that have been written to image in memory and not written yet - cleared by save()
    uint32_t lastWriteTime;        // when did the last write occure (to avoid saving to disk too often)
};

#endif // FLOPPYIMAGE_H
