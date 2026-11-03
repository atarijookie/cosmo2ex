#include <mint/osbind.h>

#include "../common/helpers.h"
#include "rec.h"

#define REC_BUFFER_SIZE     (16 * 1024)        // lines not written to the file yet - flushed on every test header

static char    *recBuf;
static int32_t  recLen;
static uint16_t recTestNo;
static char     recPath[64];
static int32_t  recWriteFails;      // failed attempts to write the log - shown on screen and at the end of the log
static int32_t  recLastErr;

static void rec_line(const char *line);

uint8_t rec_init(const char *fileName)
{
    int drive = Dgetdrv();

    recLen    = 0;
    recTestNo = 0;
    recPath[0] = 0;
    recWriteFails = 0;
    recLastErr    = 0;
    recBuf    = (char *) Malloc(REC_BUFFER_SIZE);

    if(!recBuf) {
        ce_printf("Failed to allocate the log buffer!\n");
        return 0;
    }

    while(1) {
        ce_snprintf(recPath, sizeof(recPath), "%c:\\%s", 'A' + drive, fileName);

        int32_t f = Fcreate(recPath, 0);
        if(f >= 0) {
            Fclose((int16_t) f);
            ce_printf("Log file: %s\n", recPath);
            return 1;
        }

        ce_printf("\nFailed to create %s. Drive letter for the log (ESC = no log): ", recPath);
        char key = (char) Cnecin();
        ce_printf("\n");
        if(key == 27) {
            recPath[0] = 0;
            return 1;               // run without log file, results only on screen
        }

        if(key >= 'a' && key <= 'z') key -= 32;
        if(key >= 'A' && key <= 'Z') {
            drive = key - 'A';
        }
    }
}

// append what's in the buffer to the log file - open, append, close, so it's on the host even if the ST crashes later.
// When it fails, the data stay in the buffer and the next flush tries again - nothing is dropped.
static uint8_t rec_flushTry(void)
{
    int32_t f = Fopen(recPath, 1);                  // write only
    if(f < 0) {
        recLastErr = f;
        return 0;
    }

    int32_t sk = Fseek(0, (int16_t) f, 2);          // to the end
    int32_t wr = (sk >= 0) ? Fwrite((int16_t) f, recLen, recBuf) : sk;
    Fclose((int16_t) f);

    if(wr != recLen) {                              // seek / write failed - the part written now may get written again
        recLastErr = (wr < 0) ? wr : -1;
        return 0;
    }

    return 1;
}

static void rec_flush(void)
{
    if(!recBuf || recLen == 0) {
        return;
    }

    if(!recPath[0]) {                               // no log file
        recLen = 0;
        return;
    }

    int i;
    for(i=0; i<3; i++) {                            // a few tries, then keep the data for the next flush
        if(rec_flushTry()) {
            recLen = 0;
            return;
        }

        recWriteFails++;
        ce_printf("\33p[log write failed: %d, failures: %d]\33q\n", recLastErr, recWriteFails);
        msleep(100);
    }
}

void rec_done(void)
{
    if(recWriteFails) {                             // goes to the log too, if the write works this time
        char line[64];
        ce_snprintf(line, sizeof(line), "## log write failures %d, last error %d", recWriteFails, recLastErr);
        rec_line(line);
    }

    rec_flush();

    if(recPath[0]) {
        ce_printf("\n\33pLog saved: %s\33q\n", recPath);
    }

    if(recBuf) {
        Mfree(recBuf);
    }

    recBuf = 0;
    recLen = 0;
}

// one line to the log (gets \r\n) and to the screen
static void rec_line(const char *line)
{
    (void) Cconws(line);
    (void) Cconws("\r\n");

    if(!recBuf) {
        return;
    }

    int len = strlen(line);
    if((recLen + len + 3) > REC_BUFFER_SIZE) {      // full? write it out first
        rec_flush();
    }
    if((recLen + len + 3) > REC_BUFFER_SIZE) {      // still full - writing keeps failing, the line is lost
        recWriteFails++;
        return;
    }

    memcpy(recBuf + recLen, line, len);
    recLen += len;
    recBuf[recLen++] = '\r';
    recBuf[recLen++] = '\n';
}

void rec_comment(const char *str)
{
    char line[128];
    ce_snprintf(line, sizeof(line), "## %s", str);
    rec_line(line);
}

void rec_test(uint16_t testNo, const char *name)
{
    char line[128];
    recTestNo = testNo;
    ce_snprintf(line, sizeof(line), "%04x # %s", testNo, name);
    rec_line(line);
    rec_flush();                    // previous test + this header are in the file before this test runs
}

static void rec_value(char cls, const char *key, const char *value)
{
    char line[160];
    ce_snprintf(line, sizeof(line), "%04x %c %s %s", recTestNo, cls, key, value);
    rec_line(line);
}

static void rec_int(char cls, const char *key, int32_t value)
{
    char tmp[16];
    ce_snprintf(tmp, sizeof(tmp), "%d", value);
    rec_value(cls, key, tmp);
}

// strings are recorded with non-printable chars escaped, so the line stays one line
static void rec_str(char cls, const char *key, const char *str)
{
    char tmp[100];

    if(!str) {
        rec_value(cls, key, "<null>");
        return;
    }

    int i, o = 0;
    tmp[o++] = '"';
    for(i=0; str[i] && o < (int) sizeof(tmp) - 6; i++) {
        unsigned char c = (unsigned char) str[i];

        if(c < 32 || c > 126 || c == '"' || c == '\\') {
            ce_snprintf(tmp + o, 5, "\\x%02x", c);
            o += 4;
        } else {
            tmp[o++] = c;
        }
    }
    if(str[i]) {                    // didn't fit
        tmp[o++] = '.';
        tmp[o++] = '.';
    }
    tmp[o++] = '"';
    tmp[o]   = 0;

    rec_value(cls, key, tmp);
}

void rec_x (const char *key, int32_t value)     { rec_int('=', key, value); }
void rec_f (const char *key, int32_t value)     { rec_int('~', key, value); }
void rec_xs(const char *key, const char *str)   { rec_str('=', key, str);   }
void rec_fs(const char *key, const char *str)   { rec_str('~', key, str);   }

void rec_xh(const char *key, uint32_t value)
{
    char tmp[16];
    ce_snprintf(tmp, sizeof(tmp), "0x%08x", value);
    rec_value('=', key, tmp);
}
