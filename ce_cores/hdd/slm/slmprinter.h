#ifndef SLMPRINTER_H
#define SLMPRINTER_H

#include <stdint.h>
#include <string>
#include <functional>

class AcsiDataTrans;

/*
    Emulation of the Atari SLM605 / SLM804 laser printer - the SLM page printer controller on the ACSI bus. Both printers
    connect through the same controller, the ST talks only to it - so one emulation and the same drivers are for both.
    Protocol: "The Atari Page Printer Interface" (Atari, 13 April 1988), behavior checked against DIAB630.PRG and SLM804.SYS.

    - 6 byte command, opcode in cmd[0] bits 0..4, device number in cmd[1] bits 5..7
    - status byte: device number in bits 5..7, error code in bits 0..4
    - INQUIRY / MODE SENSE: status byte followed by EXTENDED STATUS PHASE - each byte with CS / INT handshake, no DMA
    - MODE SELECT: EXTENDED COMMAND PHASE - list length byte and the list, each byte with CS / INT handshake
    - PRINT: page bitmap (1 = black) by DMA, one status byte after each page, no status for STOP PRINT

    The page data has to be taken at about the speed of the printer engine: the drivers send the page in 38400 byte bands
    and switch to the next band when the DMA address reaches band end + 32 - they wait for the exact value with interrupts off,
    and DIAB630 fails with 'page too complex' when the DMA is already past it. The device takes one scan line in a burst
    (the 1st burst has 2 more bytes - the two-byte FIFO of the controller) and then waits till the next scan line, so the
    DMA address stays at the band end long enough between the bursts. See ce_pico_hdd onDataWritePaced().
*/

// opcodes
#define SLM_OP_REQUEST_SENSE        0x03
#define SLM_OP_PRINT                0x0a
#define SLM_OP_INQUIRY              0x12
#define SLM_OP_MODE_SELECT          0x15
#define SLM_OP_MODE_SENSE           0x1a
#define SLM_OP_STOP_PRINT           0x1b

// status error codes
#define SLM_ST_OK                   0x00
#define SLM_ST_ORNERY_PRINTER       0x02
#define SLM_ST_TONER_EMPTY          0x03
#define SLM_ST_WARM_UP              0x04
#define SLM_ST_PAPER_EMPTY          0x05
#define SLM_ST_SYSTEM_TIMEOUT       0x10
#define SLM_ST_INVALID_OPCODE       0x12
#define SLM_ST_INVALID_DEVICE       0x15
#define SLM_ST_INVALID_PARAMS       0x1a

#define SLM_PARAM_LIST_LEN          23          // MODE SENSE / SELECT parameter list length - without the list length byte
#define SLM_PRINT_INDEFINITE        255         // PRINT transfer length: print pages until STOP PRINT

// INQUIRY identification string of the controller - drivers look for "PAGE PRINTER" at its start
#define SLM_ID_STRING               "PAGE PRINTER:SLMC804v1.1:ATARI "

// Time of one scan line - this paces the page data. Not documented - estimated from the print speed of SLM605 (6 pages
// per minute; SLM804 is 8). A slower scan time is safe (just slower printing), a faster one can make DIAB630 fail
// with 'page too complex'.
#define SLM_SCAN_TIME_US            2667

#define SLM_INPUT_CAPACITY          250         // pages in paper cassette (SLM804: 250, SLM605: 150)
#define SLM_OUTPUT_CAPACITY         50          // pages in output tray

#define SLM_START_DELAY_MS          500         // paper feed - wait before the 1st scan line of each page
#define SLM_STOP_PRINT_WINDOW_MS    100         // after status of a page, STOP PRINT can come in this time

// paper in the cassette - block size and margins of the print area (SLM605 / SLM804 manuals, Appendix D)
typedef struct {
    const char* key;                // value of SLM_PAPER_SIZE setting
    uint16_t    blockWidth;         // pixels
    uint16_t    blockHeight;        // scan lines
    uint16_t    leftMargin;         // pixels
    uint16_t    topMargin;          // scan lines
} SlmPaper;

#define SLM_PAPER_SETTING           "SLM_PAPER_SIZE"    // global setting (Settings page of web UI): letter, legal, a4, b5
#define SLM_PAPER_DEFAULT           "letter"            // DIAB630.PRG works only with letter width (300 bytes per line)

typedef struct {
    uint16_t blockHeight;           // scan lines
    uint16_t blockWidth;            // pixels
    uint16_t topMargin;             // scan lines
    uint16_t leftMargin;            // pixels
    uint8_t  paperFlags;            // byte 9: bit 0 = manual feed
    uint8_t  systemTimeout;         // seconds
    uint8_t  outputFlags;           // byte 23
} SlmParams;

class SlmPrinter
{
public:
    // waits up to timeoutMs for the next ACSI command from the device, fills 14 command bytes and returns true if one came
    typedef std::function<bool(uint32_t timeoutMs, uint8_t* cmd)> WaitForCommandFn;

    SlmPrinter(AcsiDataTrans* dataTrans);

    void setWaitForCommand(WaitForCommandFn fn);
    void reset(void);                           // power on state - default parameters, page count 0

    // handle one command (cmd: 14 command bytes as received from the device);
    // returns true when an unrelated command came while printing multiple pages - it's in nextCmd and wasn't handled
    bool processCommand(const uint8_t* cmd, uint8_t* nextCmd);

    //------------------
    // these are public for unit tests
    const SlmParams& getParams(void) const { return params; }

    int  buildInquiry(uint8_t* out) const;                          // identification list, returns its length
    int  buildParamList(uint8_t* out, bool absolute) const;         // list length byte + SLM_PARAM_LIST_LEN bytes, returns total length
    uint8_t applyModeSelect(const uint8_t* list, int len);          // list without its length byte, returns SLM_ST_*

    uint32_t bytesPerLine(void) const   { return (params.blockWidth + 7) / 8; }
    uint32_t pageBytes(void) const      { return bytesPerLine() * params.blockHeight; }

    const SlmPaper* getPaper(void) const { return paper; }
    void updatePaper(void);                                         // take the paper size from settings, if changed

    static const SlmPaper* paperByKey(const std::string& key);     // unknown key - default paper
    static SlmParams defaultParams(const SlmPaper* paper);
    static void bitmapToGray(const uint8_t* bitmap, uint32_t bytesPerLine, uint32_t width, uint32_t height, uint8_t* gray);
    static std::string pageFileName(time_t t, int index);          // e.g. 2026-11-02_10-42-31.png, index > 1: 2026-11-02_10-42-31_2.png

private:
    AcsiDataTrans*      dataTrans;
    SlmParams           params;
    const SlmPaper*     paper;              // paper in the cassette
    uint16_t            pageCount;          // pages printed since power on
    time_t              lastPageTime;       // page files are written in background - the last ones might not exist yet,
    int                 lastPageIndex;      // so the names used in the same second are remembered
    WaitForCommandFn    waitForCommand;

    void sendStatus(uint8_t statusByte);
    void endCommandWithoutStatus(void);

    void onInquiry(const uint8_t* cmd, uint8_t statusBase);
    void onModeSelect(const uint8_t* cmd, uint8_t statusBase);
    void onModeSense(const uint8_t* cmd, uint8_t statusBase);
    bool onPrint(const uint8_t* cmd, uint8_t statusBase, uint8_t* nextCmd);

    std::string newPagePath(void);
    void savePageAsync(const uint8_t* bitmap);
};

#endif
