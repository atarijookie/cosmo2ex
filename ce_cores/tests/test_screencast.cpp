/*
    Tests of the screencast frame chunks from hdd/acsicommand/screencastacsicommand.cpp
*/
#include <string.h>
#include <vector>

#include "doctest.h"

#include "hdd/acsicommand/screencastacsicommand.h"

// splits the frame to chunks like sendScreencastFrame() does, checks each chunk and rebuilds the frame from them
static void checkChunks(size_t frameSize, int expectedChunks)
{
    std::vector<uint8_t> frame(frameSize);
    for(size_t i = 0; i < frame.size(); i++) {
        frame[i] = (uint8_t) (i * 7);
    }

    std::vector<uint8_t> rebuilt(frameSize, 0);
    uint8_t chunk[SCREENCAST_CHUNK_HEADER + SCREENCAST_CHUNK_PAYLOAD];
    size_t offset = 0;
    int chunks = 0;

    while(true) {
        size_t len = ScreencastAcsiCommand::frameChunk(frame.data(), frameSize, 0x12345678, offset, chunk);
        if(len == 0) {
            break;
        }

        CHECK(len <= 8192);
        CHECK(len > SCREENCAST_CHUNK_HEADER);

        size_t chunkOffset = ((size_t) chunk[0] << 24) | ((size_t) chunk[1] << 16) | ((size_t) chunk[2] << 8) | chunk[3];
        size_t total       = ((size_t) chunk[4] << 24) | ((size_t) chunk[5] << 16) | ((size_t) chunk[6] << 8) | chunk[7];
        CHECK(chunkOffset == offset);
        size_t seq         = ((size_t) chunk[8] << 24) | ((size_t) chunk[9] << 16) | ((size_t) chunk[10] << 8) | chunk[11];
        CHECK(total == frameSize);
        CHECK(seq == 0x12345678);

        memcpy(rebuilt.data() + chunkOffset, chunk + SCREENCAST_CHUNK_HEADER, len - SCREENCAST_CHUNK_HEADER);
        offset += len - SCREENCAST_CHUNK_HEADER;
        chunks++;
    }

    CHECK(offset == frameSize);
    CHECK(chunks == expectedChunks);
    CHECK(rebuilt == frame);
}

TEST_CASE("frameSize - ST and not yet supported resolutions have ST screen, TT resolutions TT screen")
{
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_ST_LOW)  == 1 + 512 + 32000);
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_ST_MID)  == 1 + 512 + 32000);
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_ST_HIGH) == 1 + 512 + 32000);
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_TT_LOW)  == 1 + 512 + 153600);
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_TT_MID)  == 1 + 512 + 153600);
    CHECK(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_TT_HIGH) == SCREENCAST_FRAME_SIZE_MAX);
    CHECK(ScreencastAcsiCommand::frameSize(0x80) == 1 + 512 + 32000);      // Falcon - not supported yet
    CHECK(ScreencastAcsiCommand::frameSize(0xff) == 1 + 512 + 32000);      // unknown
}

TEST_CASE("frameChunk - chunks cover the whole ST frame, each fits in 8 KB")
{
    checkChunks(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_ST_LOW), 5);       // 4 x 8000 + 513
}

TEST_CASE("frameChunk - chunks cover the whole TT frame, offsets above 64 KB")
{
    checkChunks(ScreencastAcsiCommand::frameSize(SCREENCAST_RES_TT_LOW), 20);      // 19 x 8000 + 2113
}

TEST_CASE("frameChunk - offset past end gives no chunk")
{
    size_t size = ScreencastAcsiCommand::frameSize(SCREENCAST_RES_ST_LOW);
    std::vector<uint8_t> frame(size);
    uint8_t chunk[SCREENCAST_CHUNK_HEADER + SCREENCAST_CHUNK_PAYLOAD];
    CHECK(ScreencastAcsiCommand::frameChunk(frame.data(), size, 0, size, chunk) == 0);
}

TEST_CASE("frameSkipForFps - rounded 50 / fps, clamped to fps limits")
{
    CHECK(ScreencastAcsiCommand::frameSkipForFps(25) == 2);
    CHECK(ScreencastAcsiCommand::frameSkipForFps(10) == 5);
    CHECK(ScreencastAcsiCommand::frameSkipForFps(3)  == 17);
    CHECK(ScreencastAcsiCommand::frameSkipForFps(1)  == 50);
    CHECK(ScreencastAcsiCommand::frameSkipForFps(0)  == 50);       // below min - 1 fps
    CHECK(ScreencastAcsiCommand::frameSkipForFps(99) == 2);        // above max - 25 fps
}

TEST_CASE("live - on after live frame request, off after stop")
{
    ScreencastAcsiCommand::liveStop();
    CHECK_FALSE(ScreencastAcsiCommand::liveActive());

    ScreencastAcsiCommand::liveRequested();
    CHECK(ScreencastAcsiCommand::liveActive());
    CHECK(ScreencastAcsiCommand::liveActive());                    // stays on while within timeout

    ScreencastAcsiCommand::liveStop();
    CHECK_FALSE(ScreencastAcsiCommand::liveActive());
}
