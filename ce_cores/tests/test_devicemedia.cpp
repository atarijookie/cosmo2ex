// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests for DeviceMedia - raw disk access, here with an image file (capacity from file size) instead of a real disk

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "doctest.h"

#include "hdd/native/devicemedia.h"

TEST_CASE("DeviceMedia - image file > 4 GB: capacity from file size, sectors above 4 GB don't wrap around")
{
    char path[] = "/tmp/ce_test_devmedia_XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);

    const int64_t size = 5LL * 1024 * 1024 * 1024;     // sparse - takes no real space
    REQUIRE(ftruncate(fd, size) == 0);
    close(fd);

    DeviceMedia media;
    REQUIRE(media.iopen(path, false));

    int64_t bytes, sectors;
    media.getCapacity(bytes, sectors);
    CHECK(bytes == size);
    CHECK(sectors == size / 512);

    const int64_t high = 9000000;                       // offset 4.29 GB - above 4 GB
    const int64_t low  = high - (1LL << 32) / 512;      // the same offset, if it were cut to 32 bits

    uint8_t wr[512], rd[512];
    memset(wr, 0xA5, sizeof(wr));
    REQUIRE(media.writeSectors(high, 1, wr));

    REQUIRE(media.readSectors(high, 1, rd));
    CHECK(memcmp(rd, wr, sizeof(rd)) == 0);

    REQUIRE(media.readSectors(low, 1, rd));             // must still be empty
    uint8_t zero[512] = {0};
    CHECK(memcmp(rd, zero, sizeof(rd)) == 0);

    CHECK_FALSE(media.readSectors(sectors, 1, rd));     // past the end

    media.iclose();
    unlink(path);
}

TEST_CASE("DeviceMedia - device without capacity (character device) is not opened")
{
    DeviceMedia media;
    CHECK_FALSE(media.iopen("/dev/null", false));
    CHECK_FALSE(media.isInit());

    CHECK_FALSE(media.iopen("/dev/ce_test_not_there", false));
    CHECK_FALSE(media.isInit());
}
