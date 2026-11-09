// builtin_image.cpp - the built-in floppy: the tracks made on core 0 decode back to the disk of builtin_floppy_data.h with
// the current status in the status file + status sector, the status follows the connection, and the switch to the real
// image goes to core 1 after the tracks of that image.

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "ipc.h"
#include "settings.h"
#include "mfm_stream.h"
#include "builtin_image.h"

// what builtin_image.cpp reads from the rest of core 0 - the firmware has them in connection.cpp, settings.cpp, ...
bool connectedToWifi = false;
bool connectedToHost = false;
std::string hostIpString;
Settings_t Settings;
static bool portalActive = false;
bool captivePortalActive(void) { return portalActive; }

extern int imageState;
extern uint8_t imgTracks;
extern char imageFileName[32];
extern int receivedTracks;
extern bool diskChanged;
extern bool builtinTestMode;

namespace {

const int SPT = 9;
const int TRACKS = 80;

struct DecodedTrack {
    int track = -1;
    std::vector<int> sectors;                   // in stream order
    std::vector<std::vector<uint8_t>> data;     // 512 B each
    bool crcOk = true;
    uint32_t timeBytes = 0;                     // data bytes, for the 200 ms of a rotation
};

uint16_t crc16(uint16_t crc, uint8_t val)
{
    crc ^= ((uint16_t) val) << 8;
    for(int i=0; i<8; i++) {
        crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    }
    return crc;
}

// The stream as the WD1772 would see it: data bytes in order, A1 syncs as 0xA1 - then find IDs and data in it.
DecodedTrack decode(const uint8_t* stream, uint32_t length)
{
    DecodedTrack t;
    std::vector<uint8_t> bytes;

    uint32_t end = (stream[0] << 8) | stream[1];
    REQUIRE(stream[2] == MFM_STREAM_VERSION);
    REQUIRE(end <= length);

    uint32_t pos = MFM_STREAM_HEADER_SIZE;
    bool ended = false;

    while(pos < end && !ended) {
        uint8_t b = stream[pos++];
        if(b != MFM_ESC) {
            bytes.push_back(b);
            continue;
        }

        uint8_t op = stream[pos++];
        switch(op) {
            case MFM_OP_LITERAL:    bytes.push_back(0xF5); break;
            case MFM_OP_SYNC_A1:    bytes.push_back(0xA1); break;
            case MFM_OP_SECTOR:     pos += 3; break;
            case MFM_OP_RAW: {
                uint32_t n = (stream[pos] << 8) | stream[pos + 1];
                pos += 2;
                bytes.insert(bytes.end(), stream + pos, stream + pos + n);
                pos += n;
                break;
            }
            case MFM_OP_END:        ended = true; break;
            default:                FAIL("unexpected op " << (int) op);
        }
    }

    CHECK(ended);
    CHECK(pos == end);
    t.timeBytes = bytes.size();

    for(size_t i=0; i + 3 < bytes.size(); i++) {
        if(!(bytes[i] == 0xA1 && bytes[i + 1] == 0xA1 && bytes[i + 2] == 0xA1 && bytes[i + 3] == 0xFE)) {
            continue;
        }

        uint16_t crc = 0xffff;
        for(int j=0; j<8; j++) {
            crc = crc16(crc, bytes[i + j]);
        }
        if(crc != ((bytes[i + 8] << 8) | bytes[i + 9])) {
            t.crcOk = false;
        }

        t.track = bytes[i + 4];
        int sector = bytes[i + 6];

        size_t d = i + 10;              // data mark after the ID
        while(d + 3 < bytes.size() && !(bytes[d] == 0xA1 && bytes[d + 1] == 0xA1 && bytes[d + 2] == 0xA1 && bytes[d + 3] == 0xFB)) {
            d++;
        }
        REQUIRE(d + 4 + 512 + 2 <= bytes.size());

        crc = 0xffff;
        for(int j=0; j<4 + 512; j++) {
            crc = crc16(crc, bytes[d + j]);
        }
        if(crc != ((bytes[d + 516] << 8) | bytes[d + 517])) {
            t.crcOk = false;
        }

        t.sectors.push_back(sector);
        t.data.push_back(std::vector<uint8_t>(bytes.begin() + d + 4, bytes.begin() + d + 4 + 512));
        i = d + 517;
    }

    return t;
}

// Run the loop until nothing more comes, decode what came to core 1. Returns the commands in order.
std::vector<IpcCommand> runLoop(std::vector<DecodedTrack>& tracks, int passes = 200)
{
    std::vector<IpcCommand> commands;

    for(int i=0; i<passes; i++) {
        builtinImageLoop();

        IPCbuffer* bfr;
        while((bfr = ipcGetBufferFromFifo(1)) != NULL) {
            commands.push_back(bfr->command);

            if(bfr->command == CMD_STORE_BUILTIN_TRACK) {
                CHECK(bfr->data[0] == bfr->trackNo);
                CHECK(bfr->data[1] == 0);
                CHECK(bfr->sideNo == 0);
                uint32_t length = bfr->length;
                DecodedTrack t = decode(bfr->data + 2, length - 2);
                if(bfr->trackNo < TRACKS) {
                    CHECK(t.track == bfr->trackNo);
                }
                t.track = bfr->trackNo;
                tracks.push_back(t);
            }

            ipcBufferRelease(bfr);
        }
    }

    return commands;
}

std::vector<uint8_t> diskFromTracks(const std::vector<DecodedTrack>& tracks)
{
    std::vector<uint8_t> disk(TRACKS * SPT * 512, 0xEE);

    for(const DecodedTrack& t : tracks) {
        for(size_t i=0; i<t.sectors.size(); i++) {
            if(t.track < TRACKS && t.sectors[i] >= 1 && t.sectors[i] <= SPT) {
                memcpy(&disk[(t.track * SPT + t.sectors[i] - 1) * 512], t.data[i].data(), 512);
            }
        }
    }

    return disk;
}

const DecodedTrack* lastTrack(const std::vector<DecodedTrack>& tracks, int track)
{
    const DecodedTrack* found = NULL;
    for(const DecodedTrack& t : tracks) {
        if(t.track == track) {
            found = &t;
        }
    }
    return found;
}

std::string statusFileName(const std::vector<uint8_t>& disk)
{
    return std::string((const char*) &disk[5 * 512 + 32], 11);
}

std::string statusSectorText(const DecodedTrack& t)
{
    for(size_t i=0; i<t.sectors.size(); i++) {
        if(t.sectors[i] == 10) {
            return std::string((const char*) t.data[i].data() + 16);
        }
    }
    return "";
}

void resetAll(void)
{
    fakeReset();
    ipcInit();
    builtinImageReset();
    builtinTestMode = false;

    memset(&Settings, 0, sizeof(Settings));
    portalActive = false;
    connectedToWifi = false;
    connectedToHost = false;
    hostIpString.clear();
    imageState = IMAGE_NOT_LOADED;
    imgTracks = 0;
    receivedTracks = 0;
    imageFileName[0] = 0;
    diskChanged = false;
}

}   // namespace

TEST_CASE("builtin image: the tracks decode to the disk of builtin_floppy_data.h, with the status in it")
{
    resetAll();

    std::vector<DecodedTrack> tracks;
    std::vector<IpcCommand> commands = runLoop(tracks);

    REQUIRE(tracks.size() == MAX_TRACKS);       // the disk, then empty tracks up to the last one the head can get to
    CHECK(tracks[0].track == 0);                // track 0 first: the ST boots from it

    for(const DecodedTrack& t : tracks) {
        CAPTURE(t.track);
        CHECK(t.crcOk);
        CHECK(t.timeBytes <= 6250);             // fits into a rotation

        if(t.track == 0) {
            CHECK(t.sectors == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        } else if(t.track < TRACKS) {
            CHECK(t.sectors == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9});
        } else {
            CHECK(t.sectors.empty());
        }
    }

    std::vector<uint8_t> disk = diskFromTracks(tracks);

    // executable boot sector, BPB of a single sided 80 x 9 disk
    uint16_t sum = 0;
    for(int i=0; i<512; i+=2) {
        sum += (disk[i] << 8) | disk[i + 1];
    }
    CHECK(sum == 0x1234);
    CHECK(disk[0] == 0x60);
    CHECK((disk[0x13] | (disk[0x14] << 8)) == 720);
    CHECK(disk[0x18] == 9);
    CHECK(disk[0x1a] == 1);

    // no WiFi settings: the status file is NO_WIFI.TXT with the text, status sector the same + VT52 codes and the footer
    CHECK(statusFileName(disk) == "NO_WIFI TXT");
    uint32_t size = disk[5 * 512 + 32 + 28] | (disk[5 * 512 + 32 + 29] << 8);
    std::string text((const char*) &disk[6 * 512], size);
    CHECK(text.find("No WiFi settings stored") != std::string::npos);
    CHECK(disk[6 * 512 + size] == 0);

    std::vector<uint8_t> status = tracks[0].data[9];
    CHECK(memcmp(status.data(), "CE2XSTAT", 8) == 0);
    CHECK(status[8] == BUILTIN_STATUS_NO_WIFI);
    CHECK(statusSectorText(tracks[0]) == "\033E\033f" + text + "\r\n Waiting... press SPACE to stop.\r\n");

    // lines fit 40 columns
    size_t lineStart = 0;
    for(size_t i=0; i<=text.size(); i++) {
        if(i == text.size() || text[i] == '\r') {
            CHECK(i - lineStart <= 39);
            lineStart = i + 2;
        }
    }

    // CE_FDD.TOS: a TOS program header
    CHECK(std::string((const char*) &disk[5 * 512 + 64], 11) == "CE_FDD  TOS");
    int cluster = disk[5 * 512 + 64 + 26];
    CHECK(disk[(6 + (cluster - 2) * 2) * 512] == 0x60);
    CHECK(disk[(6 + (cluster - 2) * 2) * 512 + 1] == 0x1a);

    for(IpcCommand c : commands) {
        CHECK(c == CMD_STORE_BUILTIN_TRACK);
    }
}

TEST_CASE("builtin image: the status follows the connection, a new status remakes track 0 and changes the disk")
{
    resetAll();
    std::vector<DecodedTrack> tracks;
    runLoop(tracks);
    tracks.clear();

    // WiFi setup running: how to get to the setup page
    portalActive = true;
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_WIFI_SETUP);
    CHECK(statusFileName(diskFromTracks(tracks)) == "SETUP   TXT");
    CHECK(statusSectorText(tracks[0]).find("No WiFi settings stored yet.") != std::string::npos);
    CHECK(statusSectorText(tracks[0]).find("connect to the WiFi 'cosmo2ex'") != std::string::npos);
    CHECK(statusSectorText(tracks[0]).find("http://192.168.42.1") != std::string::npos);

    // started with the long press, settings stored - the same, without 'no settings'
    tracks.clear();
    strcpy(Settings.ssid, "homewifi");
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_WIFI_SETUP);
    CHECK(statusSectorText(tracks[0]).find("No WiFi settings") == std::string::npos);

    tracks.clear();
    diskChanged = false;
    portalActive = false;
    fakeNowMs += 1000;
    runLoop(tracks, 5);

    REQUIRE(tracks.size() == 1);                // just track 0 again
    CHECK(tracks[0].track == 0);
    CHECK(diskChanged);                         // other status file name - the ST should read the directory again
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_WIFI);
    CHECK(statusSectorText(tracks[0]).find("homewifi") != std::string::npos);
    CHECK(statusFileName(diskFromTracks(tracks)) == "WIFI    TXT");
    uint8_t counter = tracks[0].data[9][9];

    // another status file - another serial in the boot sector (TOS compares it), still executable
    std::vector<uint8_t> boot = tracks[0].data[0];
    CHECK(boot[8] == 0xce);
    CHECK(((boot[9] << 8) | boot[10]) == 3);    // no WiFi, WiFi setup, joining the WiFi
    uint16_t bootSum = 0;
    for(int i=0; i<512; i+=2) {
        bootSum += (boot[i] << 8) | boot[i + 1];
    }
    CHECK(bootSum == 0x1234);

    // connected to the WiFi, host found
    tracks.clear();
    diskChanged = false;
    connectedToWifi = true;
    hostIpString = "192.168.1.10";
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_HOST);
    CHECK(tracks[0].data[9][9] != counter);
    CHECK(statusSectorText(tracks[0]).find("192.168.1.10") != std::string::npos);

    // nothing changed - nothing made
    tracks.clear();
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    CHECK(tracks.empty());

    // loading: the progress changes only the text - no disk change, track 0 at most once a second
    connectedToHost = true;
    imageState = IMAGE_REQUESTED;
    imgTracks = 80;
    strcpy(imageFileName, "GAME.ST");
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_LOADING);
    CHECK(statusSectorText(tracks[0]).find("GAME.ST") != std::string::npos);
    CHECK(statusSectorText(tracks[0]).find(" 0 %") != std::string::npos);

    tracks.clear();
    diskChanged = false;
    receivedTracks = 80;
    fakeNowMs += 300;
    runLoop(tracks, 5);
    CHECK(tracks.empty());                      // too soon after the last one
    fakeNowMs += 700;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(statusSectorText(tracks[0]).find("50 %") != std::string::npos);
    CHECK_FALSE(diskChanged);

    // the host has no image
    tracks.clear();
    imageState = IMAGE_LOADED;
    imgTracks = 0;
    fakeNowMs += 1000;
    runLoop(tracks, 5);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_NO_IMAGE);
    CHECK(statusFileName(diskFromTracks(tracks)) == "NO_IMAGETXT");
}

TEST_CASE("builtin image: real image loaded - core 1 gets the switch after the tracks, nothing more is made")
{
    resetAll();
    std::vector<DecodedTrack> tracks;
    runLoop(tracks);

    // all core 1 buffers taken (tracks of the real image waiting in the FIFO) - the switch waits for a free one
    std::vector<IPCbuffer*> taken;
    IPCbuffer* b;
    while((b = ipcGetFreeBuffer(1, 0)) != NULL) {
        taken.push_back(b);
    }

    builtinImageRealImageLoaded();
    CHECK_FALSE(builtinImageActive());
    builtinImageLoop();
    CHECK(ipcGetBufferFromFifo(1) == NULL);

    ipcBufferRelease(taken[0]);
    tracks.clear();
    strcpy(Settings.ssid, "homewifi");          // a status change doesn't make tracks anymore
    fakeNowMs += 1000;
    std::vector<IpcCommand> commands = runLoop(tracks, 10);

    CHECK(commands == std::vector<IpcCommand>{ CMD_SHOW_REAL_IMAGE });
    CHECK(tracks.empty());
}

TEST_CASE("builtin image: test mode - all statuses in a loop with sample values, then no status sector, never switches")
{
    resetAll();
    builtinTestMode = true;
    fakeNowMs = 0;

    const uint8_t expected[8] = { BUILTIN_STATUS_WIFI_SETUP, BUILTIN_STATUS_NO_WIFI, BUILTIN_STATUS_WIFI, BUILTIN_STATUS_HOST,
                                  BUILTIN_STATUS_HOST, BUILTIN_STATUS_LOADING, BUILTIN_STATUS_NO_IMAGE, 0 };

    for(int step=0; step<8; step++) {
        CAPTURE(step);
        fakeNowMs = step * BUILTIN_TEST_STEP_MS + 100;
        std::vector<DecodedTrack> tracks;
        runLoop(tracks, (step == 0) ? 200 : 5);

        const DecodedTrack* t0 = lastTrack(tracks, 0);
        REQUIRE(t0 != NULL);
        CHECK(t0->crcOk);

        if(expected[step] == 0) {               // as after the switch to the real image
            CHECK(t0->sectors == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9});
        } else {
            REQUIRE(t0->sectors.size() == 10);
            CHECK(t0->data[9][8] == expected[step]);
            CHECK(statusSectorText(*t0).find("[test mode, step") != std::string::npos);
        }
    }

    builtinImageRealImageLoaded();
    CHECK(builtinImageActive());
    builtinTestMode = false;
}

TEST_CASE("builtin image: start makes the floppy before the main loop - core 1 not taking the tracks, the loop makes the rest")
{
    resetAll();

    builtinImageStart();                        // nobody empties the FIFO here: all buffers taken, then it gives up

    std::vector<DecodedTrack> tracks;
    runLoop(tracks);

    REQUIRE(tracks.size() == MAX_TRACKS);       // the first ones from the start, the rest from the loop - each once
    for(int i=0; i<MAX_TRACKS; i++) {
        CHECK(tracks[i].track == i);
    }
    CHECK(tracks[0].sectors.size() == 10);
}

TEST_CASE("builtin image: WiFi setup started after the real image - the built-in floppy again, until the image is loaded again")
{
    resetAll();
    std::vector<DecodedTrack> tracks;
    runLoop(tracks);

    strcpy(Settings.ssid, "homewifi");
    builtinImageRealImageLoaded();
    std::vector<IpcCommand> commands = runLoop(tracks, 5);
    CHECK(commands == std::vector<IpcCommand>{ CMD_SHOW_REAL_IMAGE });
    CHECK_FALSE(builtinImageActive());

    // nothing while the real image is shown and no WiFi setup runs
    tracks.clear();
    fakeNowMs += 1000;
    CHECK(runLoop(tracks, 5).empty());

    // WiFi setup with the button
    diskChanged = false;
    portalActive = true;
    fakeNowMs += 1000;
    commands = runLoop(tracks, 5);

    CHECK(builtinImageActive());
    CHECK(diskChanged);
    REQUIRE(commands.size() == 2);
    CHECK(commands[0] == CMD_SHOW_BUILTIN_IMAGE);
    CHECK(commands[1] == CMD_STORE_BUILTIN_TRACK);  // just track 0, with the setup status
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0].track == 0);
    CHECK(tracks[0].data[9][8] == BUILTIN_STATUS_WIFI_SETUP);
    CHECK(statusSectorText(tracks[0]).find("No WiFi settings") == std::string::npos);    // settings are stored

    // setup over, image loaded again - the real one
    portalActive = false;
    builtinImageRealImageLoaded();
    commands = runLoop(tracks, 5);
    CHECK(commands == std::vector<IpcCommand>{ CMD_SHOW_REAL_IMAGE });
    CHECK_FALSE(builtinImageActive());
}

TEST_CASE("builtin image: WiFi setup starts before core 1 got the switch to the real image - it just doesn't get it")
{
    resetAll();
    std::vector<DecodedTrack> tracks;
    runLoop(tracks);

    std::vector<IPCbuffer*> taken;              // all core 1 buffers taken - the switch can't be sent
    IPCbuffer* b;
    while((b = ipcGetFreeBuffer(1, 0)) != NULL) {
        taken.push_back(b);
    }

    builtinImageRealImageLoaded();
    builtinImageLoop();
    portalActive = true;
    builtinImageLoop();                         // switch still pending: the portal doesn't show the built-in one 'again'
    CHECK_FALSE(builtinImageActive());

    for(IPCbuffer* t : taken) {
        ipcBufferRelease(t);
    }

    std::vector<IpcCommand> commands = runLoop(tracks, 5);
    REQUIRE(commands.size() >= 2);
    CHECK(commands[0] == CMD_SHOW_REAL_IMAGE);  // sent, then the portal is seen: back to the built-in one
    CHECK(commands[1] == CMD_SHOW_BUILTIN_IMAGE);
    CHECK(builtinImageActive());
}
