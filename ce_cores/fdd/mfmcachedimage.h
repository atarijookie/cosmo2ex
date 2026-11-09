#ifndef MFMCACHEDIMAGE_H
#define MFMCACHEDIMAGE_H

#include "floppyimage.h"
#include "mfmtrackbuilder.h"
#include "../chipinterface/chipinterfacedefs.h"     // for MFM_STREAM_SIZE

// maximum 2 sides, 85 tracks per side
#define MAX_TRACKS      (2 * 85)

// Encoded track in mfmStream, as sent to the device: track #, side #, then the stream (see mfmtrackbuilder.h).
// The stream can't be longer than what's left of MFM_STREAM_SIZE after the 2 prefix bytes.
#define MFM_STREAM_PREFIX_SIZE  2
#define MFM_STREAM_MAX_BYTES    (MFM_STREAM_SIZE - MFM_STREAM_PREFIX_SIZE)

// images with more sectors per track are refused
#define MAX_SECTORS_PER_TRACK_IN_STREAM     19

typedef struct {
    volatile bool isReady;    // set to false if this track can't be streamed yet (e.g. not encoded yet or encoding at that moment)

    uint32_t encodeRequestSeq;      // incremented (under mutex) on every (re)encode request of this track - counter, not time, so 2 requests in the same ms aren't mixed up
    uint32_t encodeActionSeq;       // encodeRequestSeq value when encoder started encoding the track. If they differ at the end, another request was placed and encode needs to repeat

    int     track;
    int     side;

    uint8_t    *mfmStream;
    int     bytesInStream;          // bytes of mfmStream to send to the device - track + side prefix and the stream
} TCachedTrack;

class MfmCachedImage
{
public:
    MfmCachedImage();
    virtual ~MfmCachedImage();

    void clearWholeCachedImage(void);           // go and memset() all the cached tracks - used on new image (not on reencode)
    void storeImageParams(FloppyImage *img, int slotNo);
    bool somethingToBeEncoded(void);
    void askToReencodeTrack(int track, int side);

    bool findNotReadyTrackAndEncodeIt(FloppyImage *img, int &track, int &side);    // find a single track and encode it, or fail to find it and return false

    bool encodedTrackIsReady(int track, int side);
    uint8_t *getEncodedTrack(int track, int side, int &bytesInBuffer);
    bool copyEncodedTrackIfReady(int track, int side, uint8_t *dest, int &bytesInBuffer);  // copy whole track (MFM_STREAM_SIZE) to dest under mutex, if ready
    bool getParams(int &tracks, int &sides, int &sectorsPerTrack);
    bool isReadOnly(void);                      // the current image can't be written

    bool decodeMfmBuffer(uint8_t *inBfr, int inCnt, uint8_t *outBfr);     // decode single MFM encoded sector
    bool lastBufferWasFormatTrack(void);    // get if last decodeMfmBuffer() was sector write or format track

    bool newContent;

    static void trackAndSideToIndex(const int track, const int side, int &index);
    static void indexToTrackAndSide(const int index, int &track, int &side);

private:
    int slotNo;
    int nextIndex;      // set this to force encoding of some track sooner than would go

    bool gotImage;
    volatile bool readOnly;

    struct {
        int tracks;
        int sides;
        int spt;
    } params;

    int tracksToBeEncoded;  // holds how many tracks we still need to process

    struct {
        uint8_t *mfmData;
        int   count;

        uint8_t *pBfr;
        int  usedTimesFromByte;

        int  byteOffset;            // offset of decoded byte after 3x A1 mark
        int  bCount;                // how many bits we have now decoded in current byte
        bool remainder;             // if we got some reminder from previous decoded time
        uint8_t dByte;
        uint8_t *oBfr;                 // where the output data will be stored

        uint16_t calcedCrc;
        uint16_t recvedCrc;

        bool done;                  // set to true when received did finish this sector
        bool good;                  // status of sector decoding, which should be returned to caller
        bool isFormatTrack;         // when MFM decoder finds write to ID part of track (not data part), it's format track command
    } decoder;

    TCachedTrack tracks[MAX_TRACKS];
    uint16_t crc;       // current value of CRC calculator
    MfmTrackBuilder builder;    // stores the track being encoded

    int  getNextIndexToEncode(void);
    void encodeSingleTrack(FloppyImage *img, int side, int track, int sectorsPerTrack);

    void appendByte(uint8_t val);       // data byte which counts to the CRC
    void appendSyncA1(void);
    bool encodeSingleSector(FloppyImage *img, int side, int track, int sector, int gapBeforeSync, int gapAfterSector);

    void updateCrcSlow(uint8_t data);
    void updateCrcFast(uint8_t data);

    //-------------------
    // methods used for mfm decoding of written sector
    inline uint8_t getMfmTime(void);
    inline void addOneBit(uint8_t bit, bool newRemainder);
    inline void addTwoBits(uint8_t bits, bool newRemainder);
    inline void handleDecodedByte(void);
    //-------------------

    void dumpTracksToFile(int tracksNo, int sides, int spt);
    void log(char *str);
};

#endif // MFMCACHEDIMAGE_H
