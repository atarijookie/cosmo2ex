// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <string.h>

#include "mfmtrackbuilder.h"

#define END_MARK_SIZE   2

MfmTrackBuilder::MfmTrackBuilder()
{
    begin(NULL, 0);
}

void MfmTrackBuilder::begin(uint8_t *buffer, int bufferSize)
{
    start = buffer;
    size = bufferSize;
    used = MFM_STREAM_HEADER_SIZE;
    overflow = false;
}

// Is there room for an item of this size, with the end mark still fitting? An item is never stored partially and nothing
// is stored after an item that didn't fit, so the device gets a valid stream with just the end of the track missing.
bool MfmTrackBuilder::room(int count)
{
    if(!overflow && used + count + END_MARK_SIZE > size) {
        overflow = true;
    }

    return !overflow;
}

void MfmTrackBuilder::put(uint8_t val)
{
    start[used++] = val;
}

void MfmTrackBuilder::data(uint8_t val)
{
    if(val == MFM_ESC) {
        if(room(2)) {
            put(MFM_ESC);
            put(MFM_OP_LITERAL);
        }
    } else if(room(1)) {
        put(val);
    }
}

void MfmTrackBuilder::data(uint8_t val, int count)
{
    for(int i=0; i<count; i++) {
        data(val);
    }
}

void MfmTrackBuilder::syncA1(void)
{
    if(room(2)) {
        put(MFM_ESC);
        put(MFM_OP_SYNC_A1);
    }
}

void MfmTrackBuilder::syncC2(void)
{
    if(room(2)) {
        put(MFM_ESC);
        put(MFM_OP_SYNC_C2);
    }
}

void MfmTrackBuilder::cellTime(uint16_t time)
{
    if(room(4)) {
        put(MFM_ESC);
        put(MFM_OP_CELL_TIME);
        put(time >> 8);
        put(time & 0xff);
    }
}

void MfmTrackBuilder::sectorMark(int side, int track, int sector)
{
    if(room(5)) {
        put(MFM_ESC);
        put(MFM_OP_SECTOR);
        put(side);
        put(track);
        put(sector);
    }
}

void MfmTrackBuilder::rawRun(const uint8_t *bytes, int count)
{
    if(room(4 + count)) {
        put(MFM_ESC);
        put(MFM_OP_RAW);
        put(count >> 8);
        put(count & 0xff);

        memcpy(start + used, bytes, count);
        used += count;
    }
}

void MfmTrackBuilder::fuzzyRun(const uint8_t *dataAndMaskPairs, int count)
{
    if(room(3 + 2 * count)) {
        put(MFM_ESC);
        put(MFM_OP_FUZZY);
        put(count & 0xff);          // 256 is stored as 0

        memcpy(start + used, dataAndMaskPairs, 2 * count);
        used += 2 * count;
    }
}

int MfmTrackBuilder::finish(void)
{
    put(MFM_ESC);                   // room() always kept the space for this
    put(MFM_OP_END);

    start[0] = used >> 8;
    start[1] = used & 0xff;
    start[2] = MFM_STREAM_VERSION;
    start[3] = 0;

    return used;
}

bool MfmTrackBuilder::overflowed(void)
{
    return overflow;
}
