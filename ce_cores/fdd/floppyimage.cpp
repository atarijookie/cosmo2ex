// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <limits.h>
#include <vector>

#include "floppyimage.h"
#include "../misc/debug.h"
#include "../misc/utils.h"

FloppyImage::FloppyImage()
{
	loadedFlag      = false;
    params.isInit   = false;
    params.tracksNo         = 0;
    params.sidesNo          = 0;
    params.sectorsPerTrack  = 0;

    fajl = NULL;                            // no file open yet - close() checks this

    image.data = NULL;
    image.size = 0;

    sectorsWritten = 0;                     // nothing written yet
    lastWriteTime = Utils::getCurrentMs();  // pretend that writeSector() just happened
}

FloppyImage::~FloppyImage()
{
    // Don't call clear() here - it calls the pure virtual save(), which can't be called from base class destructor
    // (the derived part is already gone -> 'pure virtual method called' abort). Derived classes save in their own destructors.
    close();                            // if file still open, close it

    if(image.data != NULL) {            // free memory
        free(image.data);
        image.data = NULL;
    }
}

bool FloppyImage::isLoaded(void)
{
	return loadedFlag;
}

void FloppyImage::close(void)           // just close the file handle if open
{
    if(fajl) {                          // if file still open, close it
        fclose(fajl);
        fajl = NULL;
    }
}

void FloppyImage::clear(void)           // save file if needed, free the memory
{
    if(gotUnsavedChanges()) {           // if something wasn't saved yet, save it
        save();
    }

    loadedFlag = false;
    params.isInit = false;
    sectorsWritten = 0;                 // nothing written

    if(image.data != NULL) {            // free memory if should
        free(image.data);
        image.data = NULL;
        image.size = 0;
    }

    lastWriteTime = Utils::getCurrentMs();  // pretend that writeSector() just happened
}

bool FloppyImage::getParams(int &tracks, int &sides, int &sectorsPerTrack)
{
    if(!loadedFlag) {
        tracks          = 0;
        sides           = 0;
        sectorsPerTrack = 0;

        return false;
    }

    tracks          = params.tracksNo;
    sides           = params.sidesNo;
    sectorsPerTrack = params.sectorsPerTrack;

    return true;
}

bool FloppyImage::gotUnsavedChanges(void)
{
    return (sectorsWritten > 0);    // if something written and not saved, return true
}

bool FloppyImage::readNotWriteSector(bool readNotWrite, int track, int side, int sectorNo, uint8_t *buffer)
{
    if(!loadedFlag) {   // not loaded?
        logFdd(LOG_DEBUG, "FloppyImage::readNotWriteSector -- not loadedFlag");
        return false;
    }

    if(sectorNo < 1 || sectorNo > params.sectorsPerTrack) {                 // sector # out of range?
        logFdd(LOG_DEBUG, "FloppyImage::readNotWriteSector -- sector # out of range");
        return false;
    }

    if(track < 0 || track >= params.tracksNo || side < 0 || side >= params.sidesNo) {  // track / side out of range? (comes from device on write)
        logFdd(LOG_DEBUG, "FloppyImage::readNotWriteSector -- track %d / side %d out of range (image has %d tracks, %d sides)", track, side, params.tracksNo, params.sidesNo);
        return false;
    }

    int offset   = track    * (params.sidesNo * params.sectorsPerTrack);    // move to the right track
    offset      += side     * params.sectorsPerTrack;                       // then move to the right side
    offset      += (sectorNo - 1);                                          // and move a little to the right sector
    offset       = offset * 512;                                            // calculate ofsset in bytes

    if(image.data == NULL || offset < 0 || (offset + 512) > image.size) {   // geometry says more than the image really holds? don't go past the buffer
        logFdd(LOG_DEBUG, "FloppyImage::readNotWriteSector -- offset %d out of image size %d", offset, image.size);
        return false;
    }

    if(readNotWrite) {      // read - from image to buffer
        memcpy(buffer, &image.data[offset], 512);
    } else {                // write - from buffer to image
        sectorsWritten++;                       // one more unwritten sector
        lastWriteTime = Utils::getCurrentMs();  // store when the writeSector() happened
        memcpy(&image.data[offset], buffer, 512);

        logFdd(LOG_DEBUG, "FloppyImage::readNotWriteSector -- sectorsWritten: %d, lastWriteTime: %08x", sectorsWritten, lastWriteTime);
    }

    return true;
}

uint32_t FloppyImage::getLastWriteTime(void)
{
    return lastWriteTime;
}

// convenience function for reading sector
bool FloppyImage::readSector(int track, int side, int sectorNo, uint8_t *buffer)
{
    return readNotWriteSector(true, track, side, sectorNo, buffer);
}

// convenience function for writing sector
bool FloppyImage::writeSector(int track, int side, int sectorNo, uint8_t *buffer)
{
    return readNotWriteSector(false, track, side, sectorNo, buffer);
}

const char *FloppyImage::getFileName(void)
{
    return currentFileName.c_str();
}

bool FloppyImage::open(const char *fileName)
{
    close();        // close if open
    clear();        // save if needed, free memory

    currentFileName = fileName;

    fajl = fopen(fileName, "rb");

    if(fajl == NULL) {
        logFdd(LOG_ERROR, "Failed to open image file: %s", fileName);
        return false;
    }

    return true;
}

bool FloppyImage::loadImageIntoMemory(void)
{
    clear();                            // save if needed, free memory

    fseek(fajl, 0, SEEK_END);           // move to the end of file
    int cnt = ftell(fajl);              // get the file size
    fseek(fajl, 0, SEEK_SET);           // move to the start of file

    image.data = (uint8_t *)malloc(cnt);
    int res = fread(image.data, 1, cnt, fajl);
    image.size = cnt;

    if(res != cnt) {                    // failed to load? clear and fail
        close();                        // close file if open
        clear();                        // save if needed, free memory
        return false;
    }

    // leave file open - e.g. msa will need more file access, so let it and close it later
    return true;
}

bool FloppyImage::saveFileAtomically(const char *path, const uint8_t *data, size_t size)
{
    // if the image path is a symlink, replace the file it points to, not the symlink itself
    char realPath[PATH_MAX];
    if(realpath(path, realPath) == NULL) {          // file doesn't exist (yet)? just use the path as is
        snprintf(realPath, sizeof(realPath), "%s", path);
    }

    std::string tmpPath = std::string(realPath) + ".tmp-XXXXXX";
    std::vector<char> tmpName(tmpPath.begin(), tmpPath.end());
    tmpName.push_back(0);

    int fd = mkstemp(tmpName.data());               // temp file in the same dir - rename() is atomic only within one filesystem
    if(fd < 0) {
        logFdd(LOG_ERROR, "FloppyImage::saveFileAtomically - can't create temp file for %s : %s", realPath, strerror(errno));
        return false;
    }

    struct stat st;
    if(stat(realPath, &st) == 0) {                  // keep the permissions of the original file (mkstemp creates 0600)
        fchmod(fd, st.st_mode & 07777);
    } else {
        mode_t mask = umask(0);                     // new file - default permissions like fopen() would give
        umask(mask);
        fchmod(fd, 0666 & ~mask);
    }

    bool good = true;
    size_t done = 0;
    while(done < size) {                            // write() can write less than asked
        ssize_t res = write(fd, data + done, size - done);
        if(res < 0) {
            if(errno == EINTR) {
                continue;
            }
            good = false;
            break;
        }
        done += res;
    }

    if(good && fsync(fd) != 0) {                    // make sure the data is on disk before the rename makes it 'the' file
        good = false;
    }

    if(::close(fd) != 0) {
        good = false;
    }

    if(!good || rename(tmpName.data(), realPath) != 0) {   // failed? original file stays untouched, remove temp file
        logFdd(LOG_ERROR, "FloppyImage::saveFileAtomically - failed to save %s : %s", realPath, strerror(errno));
        unlink(tmpName.data());
        return false;
    }

    // make the rename itself durable - fsync the directory
    std::vector<char> dirBuf(realPath, realPath + strlen(realPath) + 1);
    int dirFd = ::open(dirname(dirBuf.data()), O_RDONLY | O_DIRECTORY);
    if(dirFd >= 0) {
        fsync(dirFd);
        ::close(dirFd);
    }

    return true;
}
