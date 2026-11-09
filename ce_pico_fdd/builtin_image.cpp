#include <cstring>
#include <cstdio>
#include <string>

#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "ipc.h"
#include "settings.h"
#include "captive_portal.h"
#include "mfm_stream.h"
#include "builtin_image.h"
#include "builtin_floppy_data.h"

extern bool connectedToWifi;
extern bool connectedToHost;
extern std::string hostIpString;

extern int imageState;
extern uint8_t imgTracks;
extern char imageFileName[32];
extern int receivedTracks;
extern bool diskChanged;

#define STATUS_SECTOR           10      // on track 0, side 0 - the BPB says 9 sectors per track, GEMDOS doesn't use it
#define STATUS_TEXT_OFFSET      16      // in the status sector
#define STATUS_CHECK_MS         250     // how often the status is looked at
#define TRACK0_REMAKE_MS        1000    // the text alone (e.g. loading progress) changes track 0 at most this often

#define TEST_STATUS_GONE        0x80    // test mode only: track 0 without the status sector, as after the switch

bool builtinTestMode = BUILTIN_IMAGE_TEST;  // statuses in a loop, never switches to the real image - see defs.h

static bool active = true;              // the ST sees the built-in floppy
static bool switchPending = false;      // switchCommand not sent to core 1 yet - no free buffer
static IpcCommand switchCommand = CMD_SHOW_REAL_IMAGE;
static int nextTrack = 0;               // the next track to make after the start, MAX_TRACKS = all made
static bool track0Dirty = true;         // status changed, track 0 has the old one
static bool codeChanged = false;        // ...and it's another status, not just another text

static uint8_t statusCode = BUILTIN_STATUS_NONE;
static uint8_t changeCounter = 0;       // in the status sector: the boot code prints the text again when it changes
static uint16_t diskSerial = 0;         // in the boot sector: another one on each status change, see getSector()
static char statusText[400];            // the status file content and the text on the ST (+ the footer)

static uint32_t lastStatusCheck = 0;
static uint32_t lastTrack0Made = 0;

bool builtinImageActive(void)
{
    return active;
}

void builtinImageRealImageLoaded(void)
{
    if(!active) {
        return;
    }

    if(builtinTestMode) {
        debug("builtin floppy: real image loaded, but in test mode - staying on the built-in floppy\n");
        return;
    }

    debug("builtin floppy: real image loaded, switching to it\n");
    active = false;
    switchPending = true;               // sent from builtinImageLoop(), after the tracks of the real image in the FIFO
    switchCommand = CMD_SHOW_REAL_IMAGE;
}

void builtinImageReset(void)
{
    active = true;
    switchPending = false;
    nextTrack = 0;
    track0Dirty = true;
    codeChanged = false;
    statusCode = BUILTIN_STATUS_NONE;
    changeCounter = 0;
    diskSerial = 0;
    statusText[0] = 0;
    lastStatusCheck = 0;
    lastTrack0Made = 0;
}

//------------------------------------------------------------------------------------------
// status

static const char* statusFileName(uint8_t code)     // 8 + 3, space padded, as in the directory entry
{
    switch(code) {
        case BUILTIN_STATUS_WIFI_SETUP: return "SETUP   TXT";
        case BUILTIN_STATUS_NO_WIFI:    return "NO_WIFI TXT";
        case BUILTIN_STATUS_WIFI:       return "WIFI    TXT";
        case BUILTIN_STATUS_HOST:       return "HOST    TXT";
        case BUILTIN_STATUS_LOADING:    return "LOADING TXT";
        case BUILTIN_STATUS_NO_IMAGE:   return "NO_IMAGETXT";
        case TEST_STATUS_GONE:          return "NO_STAT TXT";
        default:                        return "STATUS  TXT";
    }
}

// What the status is made from - the real state, or sample values in the test mode.
typedef struct {
    bool        portal;         // WiFi setup running
    const char* ssid;           // "" = no WiFi settings
    bool        wifi;           // connected to the WiFi
    bool        host;           // connected to the host
    const char* hostIp;         // "" = not found yet
    int         imageState;
    int         imgTracks;
    const char* imageName;
    int         receivedTracks;
} StatusInputs;

// The status and its text - lines of at most 39 chars, for 40 columns in ST low resolution.
static uint8_t makeStatusText(const StatusInputs& in, char* text, int size)
{
    #define HEADER  "\r\n Cosmo2ex floppy\r\n ---------------\r\n\r\n"

    if(in.portal) {     // also while it scans the networks - the AP comes up a few seconds later
        snprintf(text, size, HEADER
                 " %s"
                 " WiFi setup is running.\r\n\r\n"
                 " On a phone or a PC:\r\n"
                 " 1. connect to the WiFi '" PORTAL_AP_NAME "'\r\n"
                 " 2. open a web browser - the setup\r\n"
                 "    page opens by itself, otherwise\r\n"
                 "    go to http://192.168.42.1\r\n"
                 " 3. enter your WiFi name + password\r\n"
                 "    and press Save\r\n",
                 (in.ssid[0] == 0) ? "No WiFi settings stored yet.\r\n" : "");
        return BUILTIN_STATUS_WIFI_SETUP;
    }

    if(in.ssid[0] == 0) {           // the setup starts by itself, or retries when the AP didn't start
        snprintf(text, size, HEADER
                 " No WiFi settings stored yet.\r\n\r\n"
                 " Starting the WiFi setup...\r\n");
        return BUILTIN_STATUS_NO_WIFI;
    }

    if(!in.wifi) {
        snprintf(text, size, HEADER
                 " Connecting to the WiFi:\r\n"
                 "   %s\r\n\r\n"
                 " Taking too long? Hold the button on\r\n"
                 " the device for 1 s to set up the WiFi.\r\n", in.ssid);
        return BUILTIN_STATUS_WIFI;
    }

    if(!in.host) {
        if(in.hostIp[0] == 0) {
            snprintf(text, size, HEADER
                     " Connected to the WiFi:\r\n"
                     "   %s\r\n\r\n"
                     " Looking for the Cosmo2ex host...\r\n", in.ssid);
        } else {
            snprintf(text, size, HEADER
                     " Connected to the WiFi:\r\n"
                     "   %s\r\n\r\n"
                     " Connecting to the host:\r\n"
                     "   %s\r\n", in.ssid, in.hostIp);
        }
        return BUILTIN_STATUS_HOST;
    }

    if(in.imageState == IMAGE_LOADED && in.imgTracks == 0) {      // the host sent 'no image'
        snprintf(text, size, HEADER
                 " Connected to the host:\r\n"
                 "   %s\r\n\r\n"
                 " There's no image in the drive.\r\n"
                 " Insert one in the web UI of the host\r\n"
                 " or with the front panel.\r\n", in.hostIp);
        return BUILTIN_STATUS_NO_IMAGE;
    }

    if(in.imageState == IMAGE_REQUESTED && in.imgTracks > 0) {     // receiving the tracks - the host sends both sides always
        int percent = MIN((in.receivedTracks * 100) / (in.imgTracks * 2), 100);

        snprintf(text, size, HEADER
                 " Connected to the host:\r\n"
                 "   %s\r\n\r\n"
                 " Loading the image:\r\n"
                 "   %s\r\n"
                 "   %d %%\r\n", in.hostIp, in.imageName, percent);
    } else {
        snprintf(text, size, HEADER
                 " Connected to the host:\r\n"
                 "   %s\r\n\r\n"
                 " Loading the image...\r\n", in.hostIp);
    }
    return BUILTIN_STATUS_LOADING;
}

// Test mode: each status for BUILTIN_TEST_STEP_MS with sample values, then the status sector gone, again and again.
static uint8_t makeTestStatusText(char* text, int size)
{
    static const char* ip = "192.168.1.10";
    uint32_t step = (millis() / BUILTIN_TEST_STEP_MS) % 8;

    StatusInputs in = { false, "test-wifi", true, true, ip, IMAGE_REQUESTED, 80, "TEST.ST", 0 };

    switch(step) {
        case 0: in.portal = true; in.ssid = ""; break;      // WiFi setup, nothing stored
        case 1: in.ssid = ""; break;                        // no WiFi settings, setup not running
        case 2: in.wifi = false; break;                     // joining the WiFi
        case 3: in.host = false; in.hostIp = ""; break;     // looking for the host
        case 4: in.host = false; break;                     // connecting to it
        case 5: in.receivedTracks = 64 + ((millis() / 1000) % 8) * 8; break;  // loading, progress changing
        case 6: in.imageState = IMAGE_LOADED; in.imgTracks = 0; break;     // no image
        default:                                            // as after the switch: the boot code says 'ready'
            snprintf(text, size, "\r\n Test mode: no status sector now.\r\n");
            return TEST_STATUS_GONE;
    }

    uint8_t code = makeStatusText(in, text, size);
    int len = strlen(text);
    snprintf(text + len, size - len, "\r\n [test mode, step %d of 8]\r\n", (int) step + 1);
    return code;
}

static void checkStatus(void)
{
    char text[sizeof(statusText)];
    uint8_t code;

    if(builtinTestMode) {
        code = makeTestStatusText(text, sizeof(text));
    } else {
        StatusInputs in = { captivePortalActive(), Settings.ssid, connectedToWifi, connectedToHost, hostIpString.c_str(),
                            imageState, imgTracks, imageFileName, receivedTracks };
        code = makeStatusText(in, text, sizeof(text));
    }

    if(code == statusCode && strcmp(text, statusText) == 0) {
        return;
    }

    if(code != statusCode) {
        char name[9];
        memcpy(name, statusFileName(code), 8);
        name[8] = 0;
        for(int i=7; i>=0 && name[i] == ' '; i--) {
            name[i] = 0;
        }
        debug("builtin floppy: status %d -> %d, file %s.TXT\n", statusCode, code, name);

        diskSerial++;           // another file: another disk for TOS (it compares the serial when WP says 'maybe changed')

        if(statusCode != BUILTIN_STATUS_NONE) {     // another file name: the ST should read the directory again
            diskChanged = true;
        }

        codeChanged = true;
    }

    statusCode = code;
    strcpy(statusText, text);
    changeCounter++;
    track0Dirty = true;
}

//------------------------------------------------------------------------------------------
// tracks - v2 stream, the same layout as the host makes for .st images (see encoded_track_format.txt)

static uint8_t* out;
static int outUsed;

static inline void put(uint8_t val)
{
    out[outUsed++] = val;
}

static inline void data(uint8_t val)
{
    put(val);
    if(val == MFM_ESC) {
        put(MFM_OP_LITERAL);
    }
}

static void data(uint8_t val, int count)
{
    for(int i=0; i<count; i++) {
        data(val);
    }
}

static void syncA1(int count)
{
    for(int i=0; i<count; i++) {
        put(MFM_ESC);
        put(MFM_OP_SYNC_A1);
    }
}

static uint16_t crc16(uint16_t crc, uint8_t val)
{
    crc ^= ((uint16_t) val) << 8;

    for(int i=0; i<8; i++) {
        crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    }

    return crc;
}

static void getSector(int track, int sector, uint8_t* bfr)
{
    memset(bfr, 0, 512);

    if(track == 0 && sector == STATUS_SECTOR) {
        memcpy(bfr, "CE2XSTAT", 8);
        bfr[8] = statusCode;
        bfr[9] = changeCounter;
        snprintf((char*) bfr + STATUS_TEXT_OFFSET, 512 - STATUS_TEXT_OFFSET, "\033E\033f%s\r\n Waiting... press SPACE to stop.\r\n", statusText);
        return;
    }

    int logical = track * BUILTIN_SPT + sector - 1;

    for(int i=0; i<BUILTIN_SECTOR_COUNT; i++) {
        if(builtinSectorNo[i] == logical) {
            memcpy(bfr, builtinSectorData[i], 512);
            break;
        }
    }

    uint32_t textLen = MIN(strlen(statusText), BUILTIN_STATUS_TEXT_MAX);

    if(logical == 0) {                                      // boot sector: serial number, checksum again (executable)
        bfr[9] = diskSerial >> 8;
        bfr[10] = diskSerial;

        uint16_t sum = 0;
        for(int i=0; i<510; i+=2) {
            sum += (bfr[i] << 8) | bfr[i + 1];
        }
        sum = 0x1234 - sum;
        bfr[510] = sum >> 8;
        bfr[511] = sum;
    }

    if(logical == BUILTIN_ROOT_SECTOR) {                    // the status file: name and size
        uint8_t* entry = bfr + BUILTIN_STATUS_DIR_OFFSET;
        memcpy(entry, statusFileName(statusCode), 11);
        uint8_t* size = entry + 28;                         // little endian
        size[0] = textLen;
        size[1] = textLen >> 8;
        size[2] = textLen >> 16;
        size[3] = textLen >> 24;
    }

    int textSector = logical - BUILTIN_STATUS_TEXT_SECTOR;  // the status file data: 2 sectors
    if(textSector >= 0 && textSector * 512 < BUILTIN_STATUS_TEXT_MAX) {
        int from = textSector * 512;
        if((uint32_t) from < textLen) {
            memcpy(bfr, statusText + from, MIN(textLen - from, 512));
        }
    }
}

// Make the stream of a track (side 0) into bfr - at least READTRACKDATA_SIZE_BYTES. Returns its size.
static int makeTrack(uint8_t* bfr, int track)
{
    static uint8_t sectorData[512];
    int sectors = (track == 0 && statusCode != TEST_STATUS_GONE) ? STATUS_SECTOR : BUILTIN_SPT;  // track 0 has the status sector too: 6200 B, still fits

    out = bfr;
    outUsed = MFM_STREAM_HEADER_SIZE;

    if(track >= BUILTIN_TRACKS) {       // beyond the disk: no sectors (the PSRAM has whatever was there at power on)
        sectors = 0;
    }

    data(0x4e, 60);

    for(int sector=1; sector<=sectors; sector++) {
        put(MFM_ESC);
        put(MFM_OP_SECTOR);
        put(0);
        put(track);
        put(sector);

        // ID
        data(0x00, 12);
        syncA1(3);

        uint8_t id[5] = {0xfe, (uint8_t) track, 0, (uint8_t) sector, 2};
        uint16_t crc = 0xffff;
        for(int i=0; i<3; i++) {
            crc = crc16(crc, 0xa1);
        }
        for(int i=0; i<5; i++) {
            crc = crc16(crc, id[i]);
            data(id[i]);
        }
        data(crc >> 8);
        data(crc & 0xff);

        data(0x4e, 22);
        data(0x00, 12);
        syncA1(3);

        // data: raw run of DAM + 512 B + CRC, as the written sectors expect
        getSector(track, sector, sectorData);

        put(MFM_ESC);
        put(MFM_OP_RAW);
        put(MFM_SECTOR_RUN_SIZE >> 8);
        put(MFM_SECTOR_RUN_SIZE & 0xff);

        crc = 0xffff;
        for(int i=0; i<3; i++) {
            crc = crc16(crc, 0xa1);
        }
        crc = crc16(crc, 0xfb);
        put(0xfb);
        for(int i=0; i<512; i++) {
            crc = crc16(crc, sectorData[i]);
            put(sectorData[i]);
        }
        put(crc >> 8);
        put(crc & 0xff);

        data(0x4e, 40);
    }

    put(MFM_ESC);
    put(MFM_OP_END);

    bfr[0] = outUsed >> 8;
    bfr[1] = outUsed & 0xff;
    bfr[2] = MFM_STREAM_VERSION;
    bfr[3] = 0;
    return outUsed;
}

// Make the track and pass it to core 1. False if there's no free buffer now.
static bool sendTrack(int track)
{
    IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);
    if(bfr == NULL) {
        return false;
    }

    bfr->data[0] = track;                       // the same form as a received track: track #, side #, stream
    bfr->data[1] = 0;
    bfr->trackNo = track;
    bfr->sideNo = 0;

    int length = makeTrack(bfr->data + 2, track);
    ipcSetBufferAndPutToFifo(bfr, 1, CMD_STORE_BUILTIN_TRACK, length + 2);
    return true;
}

// The WiFi setup started while the real image is shown: the host connection is closed for it, and the ST should show how
// to get to the setup page - the built-in floppy again. After the setup the image is loaded again (reconnect), and the
// switch to it goes as after the start.
static void showBuiltinAgain(void)
{
    debug("builtin floppy: WiFi setup started - showing the built-in floppy again\n");
    active = true;

    if(switchPending && switchCommand == CMD_SHOW_REAL_IMAGE) {    // core 1 didn't switch yet - it just doesn't have to
        switchPending = false;
    } else {
        switchPending = true;
        switchCommand = CMD_SHOW_BUILTIN_IMAGE;
    }

    track0Dirty = true;         // its tracks are still in their PSRAM area, just track 0 gets the new status
    codeChanged = true;
    statusCode = BUILTIN_STATUS_NONE;
    diskChanged = true;         // another disk for the ST - and write protected again
}

void builtinImageLoop(void)
{
    if(!active && !switchPending && captivePortalActive()) {
        showBuiltinAgain();
    }

    if(switchPending) {
        IPCbuffer* bfr = ipcGetFreeBuffer(1, 0);

        if(!bfr) {
            return;
        }

        ipcSetBufferAndPutToFifo(bfr, 1, switchCommand, 0);
        switchPending = false;
    }

    if(!active) {
        return;
    }

    uint32_t now = millis();

    if(statusCode == BUILTIN_STATUS_NONE || (now - lastStatusCheck) >= STATUS_CHECK_MS) {
        lastStatusCheck = now;
        checkStatus();
    }

    if(track0Dirty && (codeChanged || nextTrack == 0 || (now - lastTrack0Made) >= TRACK0_REMAKE_MS)) {
        if(sendTrack(0)) {
            debug("builtin floppy: track 0 made - status %d, change counter %d\n", statusCode, changeCounter);
            track0Dirty = false;
            codeChanged = false;
            lastTrack0Made = now;
            nextTrack = MAX(nextTrack, 1);
        }
        return;
    }

    if(nextTrack > 0 && nextTrack < MAX_TRACKS) {   // the rest of the disk after the start, one track per pass
        if(sendTrack(nextTrack)) {
            nextTrack++;
        }
    }
}

// At the start, before the main loop: the whole built-in floppy, waiting for core 1 to store the tracks. The main loop
// blocks for seconds while joining the WiFi / connecting to the host, and the ST reads the boot sector a few seconds
// after the power on - so the floppy must be there before that.
void builtinImageStart(void)
{
    uint32_t start = millis();
    checkStatus();

    for(int track=0; track<MAX_TRACKS; track++) {
        while(!sendTrack(track)) {          // no free buffer - core 1 frees them as it stores the tracks
            if((millis() - start) >= 5000) {
                debug("builtin floppy: core 1 doesn't take the tracks, %d made - the main loop makes the rest\n", track);
                nextTrack = MAX(track, 1);
                track0Dirty = (track == 0);
                return;
            }
            sleep_ms(1);
        }
    }

    nextTrack = MAX_TRACKS;
    track0Dirty = false;
    codeChanged = false;
    lastTrack0Made = millis();

    diskChanged = true;     // an ST that read the drive before this got garbage - the disk change makes GEMDOS read it again

    debug("builtin floppy: %d tracks made in %d ms, the ST sees the built-in floppy now (status %d%s)\n",
          MAX_TRACKS, (int) (millis() - start), statusCode, builtinTestMode ? ", TEST MODE" : "");
}
