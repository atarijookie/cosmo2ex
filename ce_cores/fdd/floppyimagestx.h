// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef FLOPPYIMAGESTX_H
#define FLOPPYIMAGESTX_H

#include <stdint.h>
#include <vector>
#include "floppyimage.h"

// STX (Pasti) image: not a list of sectors, but a description of each track - the sectors with their position on the track,
// their status from the FDC, unstable (fuzzy) bits, bit rate changes, and optionally the track as Read Track returned it.
// That's what the copy protections live on, so the device gets the track rebuilt from it. Read only.
class FloppyImageStx: public FloppyImage
{
public:
    virtual ~FloppyImageStx();

    virtual bool open(const char *fileName);
    virtual bool save(void);                    // nothing to save, writes are refused
    virtual bool readNotWriteSector(bool readNotWrite, int track, int side, int sectorNo, uint8_t *buffer);

    virtual bool isReadOnly(void);
    virtual bool buildTrack(MfmTrackBuilder &builder, int track, int side);

private:
    struct Sector {
        uint16_t bitPosition;       // position of the end of ID mark on the track, in bits
        uint16_t readTime;          // us for the whole sector, 0 = standard
        uint8_t  idTrack, idHead, idSector, idSize;
        uint16_t idCrc;
        uint8_t  status;

        int      size;              // bytes of data, 0 = this sector has no data
        const uint8_t *data;
        const uint8_t *fuzzyMask;   // NULL if no fuzzy bits
        const uint8_t *timing;      // NULL if standard: 2 bytes for each 16 bytes of data
        int      timingBlocks;
    };

    struct Track {
        bool present;
        int  mfmSize;               // length of the track in bytes
        const uint8_t *image;       // optional track image, as returned by Read Track
        int  imageSize;
        std::vector<Sector> sectors;
    };

    std::vector<Track> tracks;      // index: 2 * track + side

    bool parse(void);
    bool parseTrack(uint32_t blockStart, uint32_t blockSize, int revision);
};

#endif // FLOPPYIMAGESTX_H
