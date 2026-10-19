/*
    Tests of the screencast frame chunks from hdd/acsicommand/screencastacsicommand.cpp
*/
#include <string.h>
#include <vector>

#include "doctest.h"

#include "hdd/acsicommand/screencastacsicommand.h"

TEST_CASE("frameChunk - chunks cover the whole frame, each fits in 8 KB")
{
    std::vector<uint8_t> frame(SCREENCAST_FRAME_SIZE);
    for(size_t i = 0; i < frame.size(); i++) {
        frame[i] = (uint8_t) (i * 7);
    }

    std::vector<uint8_t> rebuilt(SCREENCAST_FRAME_SIZE, 0);
    uint8_t chunk[SCREENCAST_CHUNK_HEADER + SCREENCAST_CHUNK_PAYLOAD];
    size_t offset = 0;
    int chunks = 0;

    while(true) {
        size_t len = ScreencastAcsiCommand::frameChunk(frame.data(), offset, chunk);
        if(len == 0) {
            break;
        }

        CHECK(len <= 8192);
        CHECK(len > SCREENCAST_CHUNK_HEADER);

        size_t chunkOffset = (chunk[0] << 8) | chunk[1];
        size_t total = (chunk[2] << 8) | chunk[3];
        CHECK(chunkOffset == offset);
        CHECK(total == SCREENCAST_FRAME_SIZE);

        memcpy(rebuilt.data() + chunkOffset, chunk + SCREENCAST_CHUNK_HEADER, len - SCREENCAST_CHUNK_HEADER);
        offset += len - SCREENCAST_CHUNK_HEADER;
        chunks++;
    }

    CHECK(offset == SCREENCAST_FRAME_SIZE);
    CHECK(chunks == 5);                     // 4 x 8000 + 33
    CHECK(rebuilt == frame);
}

TEST_CASE("frameChunk - offset past end gives no chunk")
{
    std::vector<uint8_t> frame(SCREENCAST_FRAME_SIZE);
    uint8_t chunk[SCREENCAST_CHUNK_HEADER + SCREENCAST_CHUNK_PAYLOAD];
    CHECK(ScreencastAcsiCommand::frameChunk(frame.data(), SCREENCAST_FRAME_SIZE, chunk) == 0);
}
