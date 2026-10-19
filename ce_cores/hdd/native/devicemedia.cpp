#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#if defined(__linux__)
#include <linux/fs.h>           // BLKGETSIZE64
#elif defined(__APPLE__)
#include <sys/disk.h>           // DKIOCGETBLOCKCOUNT, DKIOCGETBLOCKSIZE
#endif

#include "devicemedia.h"
#include "../../misc/debug.h"

// offsets on disks > 2 GB don't fit in 32 bits - off_t is 64 bit on macOS, Cygwin and 64 bit Linux,
// on 32 bit Linux thanks to _FILE_OFFSET_BITS=64 (makefile)
static_assert(sizeof(off_t) >= 8, "off_t must be 64 bit - build with -D_FILE_OFFSET_BITS=64");

// capacity of the opened device in bytes, false (and errno set) if it can't be found out
static bool deviceCapacity(int fd, uint64_t &bytes)
{
    struct stat st;
    if(fstat(fd, &st) < 0) {
        return false;
    }

    if(S_ISREG(st.st_mode)) {           // image file instead of a device - its size, on every system
        bytes = (uint64_t) st.st_size;
        return true;
    }

#if defined(__linux__)
    return ioctl(fd, BLKGETSIZE64, &bytes) == 0;
#elif defined(__APPLE__)
    uint64_t blockCount = 0;
    uint32_t blockSize  = 0;

    if(ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) < 0) {
        return false;
    }

    bytes = blockCount * blockSize;     // block size is not always 512 (e.g. 4096 on some disks)
    return true;
#else
    errno = ENOTSUP;                    // e.g. Windows (Cygwin) - raw disks not supported, only image files
    return false;
#endif
}

DeviceMedia::DeviceMedia()
{
    fdes            = -1;
    BCapacity       = 0;
    SCapacity       = 0;
    mediaHasChanged = 0;
}

DeviceMedia::~DeviceMedia()
{
    iclose();
}

bool DeviceMedia::iopen(const char *path, bool createIfNotExists)
{
    iclose();                                       // close previously opened device, if any

    fdes = open(path, O_RDWR);                      // open device for Read / Write

    if(fdes < 0) {                                  // failed to open for R/W?
        fdes = open(path, O_RDONLY);                // try to open the device for reading only

        if(fdes < 0) {                              // failed to open as read only? damn...
            logHdd(LOG_ERROR, "DeviceMedia - failed to open %s : %s", path, strerror(errno));
            return false;
        }
    }

    uint64_t sizeBytes = 0;
    if(!deviceCapacity(fdes, sizeBytes)) {          // failed to get capacity?
        logHdd(LOG_ERROR, "DeviceMedia - failed to get capacity of %s : %s", path, strerror(errno));
        iclose();                                   // don't leak the fd and don't stay 'init'
        return false;
    }

    BCapacity = (int64_t) sizeBytes;                // capacity in bytes
    SCapacity = BCapacity / 512;                    // capacity in sectors

    mediaHasChanged = false;

    int64_t capacityInMB = BCapacity / ((int64_t) (1024 * 1024));

    logHdd(LOG_DEBUG, "DeviceMedia - open succeeded, capacity: %lld MB, sectors: %lld", (long long) capacityInMB, (long long) SCapacity);

    return true;
}

void DeviceMedia::iclose(void)
{
    if(fdes >= 0) {
        close(fdes);                            // close device
    }

    fdes            = -1;
    BCapacity       = 0;
    SCapacity       = 0;
    mediaHasChanged = 0;
}

bool DeviceMedia::isInit(void)
{
    if(fdes >= 0) {
        return true;
    }

    return false;
}

bool DeviceMedia::mediaChanged(void)
{
    return mediaHasChanged;
}

void DeviceMedia::setMediaChanged(bool changed)
{
    mediaHasChanged = changed;
}

void DeviceMedia::getCapacity(int64_t &bytes, int64_t &sectors)
{
    bytes   = BCapacity;
    sectors = SCapacity;
}

bool DeviceMedia::readSectors(int64_t sectorNo, uint32_t count, uint8_t *bfr)
{
    if(!isInit()) {                             // if not initialized, failed
        logHdd(LOG_DEBUG, "DeviceMedia::readSectors - not init");
        return false;
    }

    off_t pos = (off_t) sectorNo * 512;         // convert sector # to offset
    off_t ofs = lseek(fdes, pos, SEEK_SET);
    if(ofs != pos) {
        logHdd(LOG_ERROR, "DeviceMedia::readSectors - lseek failed %lld : %s", (long long)ofs, strerror(errno));
        return false;
    }

    size_t byteCount = count * 512;
	size_t cnt = read(fdes, bfr, byteCount);	// try to read sector(s)

    if(cnt != byteCount) {                      // not all data was read? fail
        logHdd(LOG_DEBUG, "DeviceMedia::readSectors - read() failed");
        return false;
    }

    return true;
}

bool DeviceMedia::writeSectors(int64_t sectorNo, uint32_t count, uint8_t *bfr)
{
    if(!isInit()) {                             // if not initialized, failed
        logHdd(LOG_DEBUG, "DeviceMedia::writeSectors - not init");
        return false;
    }

    off_t pos = (off_t) sectorNo * 512;         // convert sector # to offset
    off_t ofs = lseek(fdes, pos, SEEK_SET);
    if(ofs != pos) {
        logHdd(LOG_ERROR, "DeviceMedia::writeSectors - lseek failed %lld : %s", (long long)ofs, strerror(errno));
        return false;
    }

    size_t byteCount = count * 512;
	size_t cnt = write(fdes, bfr, byteCount);	// try to write sector(s)

    if(cnt != byteCount) {                      // not all data was writen? fail
        logHdd(LOG_DEBUG, "DeviceMedia::writeSectors - write() failed");
        return false;
    }

    return true;
}
