//--------------------------------------------------
// Test 06 - Fread / Fseek / Fwrite through the CE_DD file buffers
//
// CE_DD keeps a 512 byte read buffer per file, serves small Freads and SEEK_CUR seeks from it, and splits big
// Freads into buffered + direct DMA parts. These tests mix the calls in ways which cross the buffer edges and
// verify every returned value and every byte against a model (shadow copy of the file in RAM).
//
//  0601 - 0608 : scripted cases (Signum font loading pattern, known suspicious driver paths)
//  0611 - 061A : edge cases where TOS versions might differ - expected values come from edgeCases[] table,
//                run the test on a native TOS drive (floppy / GEMDOS partition) to see what that TOS returns
//  0620 - 0623 : random read + seek sequences (fixed seeds, so they repeat exactly)
//  0630 - 0631 : random read + write + seek sequences
//
// Error code on failed line: RRoooooo - RR = reason (see FAIL_* below), oooooo = index of the failing operation.
// Details and the last operations are shown below the failed line, written to the FSYS_xxx.TXT log file
// and (with CE_DD supporting CELG) sent to the host log - core_hdd.log, lines 'ST log: T06 ...'.
//--------------------------------------------------
#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
#include <support.h>

#include <stdio.h>
#include "../common/helpers.h"
#include "out.h"

extern int  drive;
extern WORD tosVersion;

#define FAIL_NONE        0
#define FAIL_OPEN        1      // Fopen / Fcreate failed
#define FAIL_FREAD_RET   2      // Fread returned other count than expected
#define FAIL_DATA        3      // Fread returned wrong data
#define FAIL_GUARD       4      // Fread wrote outside of the requested buffer
#define FAIL_FSEEK_RET   5      // Fseek returned other position / error than expected
#define FAIL_FWRITE_RET  6      // Fwrite returned other count than expected
#define FAIL_VERIFY      7      // file content / size after close doesn't match
#define FAIL_EDGE        8      // edge case returned other value than in the table

static const char *failNames[] = {"-", "open", "Fread ret", "data", "guard", "Fseek ret", "Fwrite ret", "verify", "edge"};

#define T06_FILE        "\\T06_FIO.BIN"
#define RND_FILESIZE    23456               // odd size, many 512 byte buffers long
#define SHADOW_SIZE     (RND_FILESIZE + 16384)
#define IO_MAX          8192
#define GUARD           16
#define GUARD_BYTE      0xA5
#define IOBUF_SIZE      (GUARD + 4 + IO_MAX + GUARD + 4)

#define HIST_COUNT      6
#define HIST_LEN        72

//--------------------------------------------------
// state of the currently running test case
static BYTE  *shadow;           // what the file should contain
static BYTE  *ioRaw, *ioBuf;    // ST RAM buffer (ioBuf is longword aligned)
static BYTE  *ttRaw, *ttBuf;    // TT RAM buffer (if available)
static BYTE  *curBuf;           // which of the above is used for the I/O now

static int    fh;               // file handle
static DWORD  fpos;             // where we think the file pointer is
static DWORD  fsize;            // what we think the file size is
static DWORD  opIndex;          // index of the operation in the current test case

static BYTE   failReason;
static DWORD  failOp;
static char   failMsg[2][HIST_LEN];
static char   hist[HIST_COUNT][HIST_LEN];   // ring buffer of the last operations
static WORD   histCnt;

static uint32_t rndState;

//--------------------------------------------------
static uint32_t rnd(void)                   // xorshift32 - no multiplication needed on 68000
{
    uint32_t x = rndState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rndState = x;
    return x;
}

static BYTE patternByte(DWORD offset, WORD seed)    // file content generator - neighbouring bytes always differ
{
    return (BYTE) ((offset * 7) + (offset >> 8) * 13 + (offset >> 16) + seed);
}

static void fillPattern(BYTE *bfr, DWORD offset, DWORD count, WORD seed)
{
    DWORD i;
    for(i=0; i<count; i++) {
        bfr[i] = patternByte(offset + i, seed);
    }
}

//--------------------------------------------------
static void caseStart(void)
{
    opIndex     = 0;
    failReason  = FAIL_NONE;
    failOp      = 0;
    histCnt     = 0;
    failMsg[0][0] = 0;
    failMsg[1][0] = 0;
    fh          = -1;
    curBuf      = ioBuf;
}

static void histAdd(const char *str)
{
    strncpy(hist[histCnt % HIST_COUNT], str, HIST_LEN - 1);
    hist[histCnt % HIST_COUNT][HIST_LEN - 1] = 0;
    histCnt++;
}

static BYTE fail(BYTE reason, const char *msg)
{
    if(failReason == FAIL_NONE) {       // keep only the first failure
        failReason = reason;
        failOp     = opIndex;
        strncpy(failMsg[0], msg, HIST_LEN - 1);
        failMsg[0][HIST_LEN - 1] = 0;
    }
    return 0;
}

static void failMsg2(const char *msg)
{
    strncpy(failMsg[1], msg, HIST_LEN - 1);
    failMsg[1][HIST_LEN - 1] = 0;
}

// show result line on screen + log file, details on failure, everything also to the host log
static void caseEnd(WORD testNo, char *name)
{
    char line[160];
    BYTE ok = (failReason == FAIL_NONE);

    if(fh >= 0) {                       // test case left the file open? close it
        Fclose(fh);
        fh = -1;
    }

    DWORD code = ok ? 0 : ((((DWORD) failReason) << 24) | (failOp & 0xffffff));
    out_tr_bd(testNo, name, ok, code);

    ce_snprintf(line, sizeof(line), "T06 [%04x] %s : %s, ops: %lu", testNo, name, ok ? "OK" : "FAIL", opIndex);
    hostLog(line);

    if(ok) {
        return;
    }

    ce_snprintf(line, sizeof(line), "    reason %d (%s) at op %lu", failReason, failNames[failReason], failOp);
    out_s(line);
    hostLog(line);

    int i;
    for(i=0; i<2; i++) {
        if(failMsg[i][0]) {
            ce_snprintf(line, sizeof(line), "    %s", failMsg[i]);
            out_s(line);
            hostLog(line);
        }
    }

    // last operations, oldest first
    WORD cnt   = (histCnt < HIST_COUNT) ? histCnt : HIST_COUNT;
    WORD first = histCnt - cnt;
    for(i=0; i<cnt; i++) {
        ce_snprintf(line, sizeof(line), "    #%u %s", first + i + 1, hist[(first + i) % HIST_COUNT]);     // entry N is op N+1
        out_s(line);
        hostLog(line);
    }
}

//--------------------------------------------------
// file operations which check themselves against the model
static BYTE opOpen(WORD mode)
{
    fh = Fopen(T06_FILE, mode);

    if(fh < 0) {
        char tmp[HIST_LEN];
        ce_snprintf(tmp, sizeof(tmp), "Fopen mode %d -> %d", mode, fh);
        fh = -1;
        return fail(FAIL_OPEN, tmp);
    }

    fpos = 0;
    return 1;
}

static void opClose(void)
{
    if(fh >= 0) {
        Fclose(fh);
        fh = -1;
    }
}

// read n bytes to curBuf + align, check returned count, data and guard bytes around the buffer
static BYTE opRead(DWORD n, WORD align)
{
    char tmp[HIST_LEN];
    BYTE *dst = curBuf + GUARD + align;
    DWORD i;

    if(failReason != FAIL_NONE) {
        return 0;
    }

    opIndex++;
    memset(curBuf, GUARD_BYTE, GUARD + align + n + GUARD);

    int32_t res = Fread(fh, n, dst);
    int32_t exp = (fpos >= fsize) ? 0 : (((fsize - fpos) < n) ? (int32_t) (fsize - fpos) : (int32_t) n);

    ce_snprintf(tmp, sizeof(tmp), "Fread  n=%lu buf+%d %s @%lu -> %ld", n, align, (curBuf == ttBuf) ? "TT" : "ST", fpos, res);
    histAdd(tmp);

    if(res != exp) {
        ce_snprintf(tmp, sizeof(tmp), "Fread n=%lu @%lu returned %ld, expected %ld", n, fpos, res, exp);
        return fail(FAIL_FREAD_RET, tmp);
    }

    for(i=0; i<(DWORD) res; i++) {
        if(dst[i] != shadow[fpos + i]) {
            ce_snprintf(tmp, sizeof(tmp), "data @%lu (buf idx %lu): got %02x exp %02x", fpos + i, i, dst[i], shadow[fpos + i]);
            fail(FAIL_DATA, tmp);

            // is the data just shifted? that tells which byte got lost / duplicated
            int sh;
            for(sh=-2; sh<=2; sh++) {
                if(sh == 0 || ((int32_t) (fpos + i) + sh) < 0 || (fpos + i + sh + 8) > fsize || (i + 8) > (DWORD) res) {
                    continue;
                }
                if(memcmp(&dst[i], &shadow[fpos + i + sh], 8) == 0) {
                    ce_snprintf(tmp, sizeof(tmp), "data from there on is file data shifted by %d", sh);
                    failMsg2(tmp);
                    break;
                }
            }
            return 0;
        }
    }

    for(i=0; i<(GUARD + align); i++) {                  // nothing written before the buffer?
        if(curBuf[i] != GUARD_BYTE) {
            ce_snprintf(tmp, sizeof(tmp), "byte %ld before buffer overwritten (%02x)", (int32_t) i - (int32_t) (GUARD + align), curBuf[i]);
            return fail(FAIL_GUARD, tmp);
        }
    }

    for(i=0; i<GUARD; i++) {                            // nothing written after the requested count?
        if(dst[n + i] != GUARD_BYTE) {
            ce_snprintf(tmp, sizeof(tmp), "byte %lu after buffer end overwritten (%02x)", i, dst[n + i]);
            return fail(FAIL_GUARD, tmp);
        }
    }

    fpos += res;
    return 1;
}

static int32_t seekTarget(int32_t offset, WORD mode)
{
    int32_t base = (mode == 0) ? 0 : ((mode == 1) ? (int32_t) fpos : (int32_t) fsize);
    return base + offset;
}

// seek which must succeed (target inside the file)
static BYTE opSeek(int32_t offset, WORD mode)
{
    char tmp[HIST_LEN];
    static const char *modes[3] = {"SET", "CUR", "END"};

    if(failReason != FAIL_NONE) {
        return 0;
    }

    opIndex++;
    int32_t exp = seekTarget(offset, mode);
    int32_t res = Fseek(offset, fh, mode);

    ce_snprintf(tmp, sizeof(tmp), "Fseek  %ld %s @%lu -> %ld", offset, modes[mode], fpos, res);
    histAdd(tmp);

    if(res != exp) {
        ce_snprintf(tmp, sizeof(tmp), "Fseek(%ld, %s) @%lu returned %ld, expected %ld", offset, modes[mode], fpos, res, exp);
        return fail(FAIL_FSEEK_RET, tmp);
    }

    fpos = res;
    return 1;
}

static BYTE opTell(void)
{
    return opSeek(0, 1);
}

// write n bytes of new data (seed) at the current position
static BYTE opWrite(DWORD n, WORD align, WORD seed)
{
    char tmp[HIST_LEN];
    BYTE *src = curBuf + GUARD + align;

    if(failReason != FAIL_NONE) {
        return 0;
    }

    if(fpos + n > SHADOW_SIZE) {            // keep the model inside the shadow buffer
        n = SHADOW_SIZE - fpos;
    }

    opIndex++;
    fillPattern(src, fpos, n, seed);
    int32_t res = Fwrite(fh, n, src);

    ce_snprintf(tmp, sizeof(tmp), "Fwrite n=%lu buf+%d @%lu -> %ld", n, align, fpos, res);
    histAdd(tmp);

    if(res != (int32_t) n) {
        ce_snprintf(tmp, sizeof(tmp), "Fwrite n=%lu @%lu returned %ld", n, fpos, res);
        return fail(FAIL_FWRITE_RET, tmp);
    }

    memcpy(&shadow[fpos], src, n);
    fpos += n;
    if(fpos > fsize) {
        fsize = fpos;
    }
    return 1;
}

//--------------------------------------------------
// create the test file from the shadow buffer (simple sequential writes)
static BYTE createTestFile(void)
{
    char tmp[HIST_LEN];

    Fdelete(T06_FILE);
    fh = Fcreate(T06_FILE, 0);
    if(fh < 0) {
        ce_snprintf(tmp, sizeof(tmp), "Fcreate -> %d", fh);
        fh = -1;
        return fail(FAIL_OPEN, tmp);
    }

    DWORD done = 0;
    while(done < fsize) {
        DWORD n = ((fsize - done) > 4096) ? 4096 : (fsize - done);
        int32_t res = Fwrite(fh, n, &shadow[done]);
        if(res != (int32_t) n) {
            ce_snprintf(tmp, sizeof(tmp), "create: Fwrite n=%lu @%lu -> %ld", n, done, res);
            opClose();
            return fail(FAIL_FWRITE_RET, tmp);
        }
        done += n;
    }

    opClose();
    return 1;
}

static BYTE createPatternFile(DWORD size, WORD seed)
{
    fsize = size;
    fillPattern(shadow, 0, size, seed);
    return createTestFile();
}

// after close: reopen, check size and whole content in 4 kB reads (to the ST RAM buffer)
static BYTE verifyFileAfterClose(void)
{
    char tmp[HIST_LEN];

    if(failReason != FAIL_NONE) {
        return 0;
    }

    opClose();
    if(!opOpen(0)) {
        return 0;
    }

    int32_t size = Fseek(0, fh, 2);
    Fseek(0, fh, 0);

    if(size != (int32_t) fsize) {
        ce_snprintf(tmp, sizeof(tmp), "file size after close %ld, expected %lu", size, fsize);
        opClose();
        return fail(FAIL_VERIFY, tmp);
    }

    DWORD done = 0;
    while(done < fsize) {
        DWORD n = ((fsize - done) > 4096) ? 4096 : (fsize - done);
        int32_t res = Fread(fh, n, ioBuf);
        if(res != (int32_t) n) {
            ce_snprintf(tmp, sizeof(tmp), "verify: Fread n=%lu @%lu -> %ld", n, done, res);
            opClose();
            return fail(FAIL_VERIFY, tmp);
        }

        DWORD i;
        for(i=0; i<n; i++) {
            if(ioBuf[i] != shadow[done + i]) {
                ce_snprintf(tmp, sizeof(tmp), "verify: data @%lu got %02x exp %02x", done + i, ioBuf[i], shadow[done + i]);
                opClose();
                return fail(FAIL_VERIFY, tmp);
            }
        }
        done += n;
    }

    opClose();
    return 1;
}

//--------------------------------------------------
// 0601 - Signum 3 font loading, as seen in host log when loading AKZ___11.S01 failed with checksum error.
// File = chunks of: 00 00, 8 char name, 4 byte BE length, 2 bytes, data[length]
static const char  *fontChunkNames[5] = {"chset001", "fdeskr01", "lgtab001", "chars001", "kerntab1"};
static const DWORD  fontChunkLens[5]  = {64, 256, 512, 7090, 1584};

static void test0601(void)
{
    WORD align, c;

    caseStart();

    // build the font-like file
    DWORD p = 0;
    for(c=0; c<5; c++) {
        fillPattern(&shadow[p], p, 16 + fontChunkLens[c], 0x5A);
        shadow[p + 0] = 0;
        shadow[p + 1] = 0;
        memcpy(&shadow[p + 2], fontChunkNames[c], 8);
        shadow[p + 10] = fontChunkLens[c] >> 24;
        shadow[p + 11] = fontChunkLens[c] >> 16;
        shadow[p + 12] = fontChunkLens[c] >>  8;
        shadow[p + 13] = fontChunkLens[c];
        p += 16 + fontChunkLens[c];
    }
    fsize = p;                                  // 9586

    if(!createTestFile()) {
        caseEnd(0x0601, "Signum font load pattern");
        return;
    }

    for(align=0; align<4 && failReason == FAIL_NONE; align++) {
        // pass 1 - open, close
        if(!opOpen(0)) break;
        opClose();

        // pass 2 - walk the chunk headers: read header, seek to chunk start, skip chunk with SEEK_CUR, tell
        if(!opOpen(0)) break;
        while(failReason == FAIL_NONE && fpos < fsize) {
            DWORD start = fpos;
            if(!opRead(16, align)) break;
            BYTE *h = curBuf + GUARD + align;
            DWORD len = ((DWORD) h[10] << 24) | ((DWORD) h[11] << 16) | ((DWORD) h[12] << 8) | h[13];
            opSeek(start, 0);
            opSeek(16 + len, 1);
            opTell();
        }
        opClose();

        // pass 3 - go to 2nd chunk (SET 0 + CUR 80), tell, then read the chunks: header + data
        if(!opOpen(0)) break;
        opSeek(0, 0);
        opSeek(80, 1);
        opTell();
        for(c=1; c<5 && failReason == FAIL_NONE; c++) {
            opRead(16, align);
            opRead(fontChunkLens[c], align);
        }
        opRead(16, align);                      // at EOF - must return 0
        opSeek(352, 0);
        opClose();
    }

    caseEnd(0x0601, "Signum font load pattern");
}

// 0602 - read, seek back inside the read buffer (SEEK_CUR), then a big read up to EOF
static void test0602(void)
{
    WORD align;
    caseStart();

    if(createPatternFile(RND_FILESIZE, 1) && opOpen(0)) {
        for(align=0; align<4 && failReason == FAIL_NONE; align++) {
            opSeek(fsize - 1500, 0);
            opRead(100, align);
            opSeek(-50, 1);                     // back, still inside the buffer
            opRead(2000, align);                // big read, should return everything to EOF (1450)
            opRead(10, align);                  // at EOF
        }
    }

    caseEnd(0x0602, "Seek back in buffer, big read to EOF");
}

// 0603 - read, seek forward inside the read buffer (SEEK_CUR), then a big read up to EOF
static void test0603(void)
{
    WORD align;
    caseStart();

    if(createPatternFile(RND_FILESIZE, 2) && opOpen(0)) {
        for(align=0; align<4 && failReason == FAIL_NONE; align++) {
            opSeek(fsize - 1500, 0);
            opRead(100, align);
            opSeek(200, 1);                     // forward, still inside the buffer
            opRead(2000, align);                // big read, should return everything to EOF (1200)
            opRead(10, align);
        }
    }

    caseEnd(0x0603, "Seek fwd in buffer, big read to EOF");
}

// 0604 - small read fills the buffer close to EOF, then big read with odd / even buffer and odd / even rest in buffer.
// This makes fread_big() skip its middle (DMA) phase, which is where the 1 byte kept back for ODD buffer is re-read.
static void test0604(void)
{
    WORD align, first;
    DWORD dist;
    caseStart();

    if(createPatternFile(RND_FILESIZE, 3) && opOpen(0)) {
        for(dist=600; dist<=1000 && failReason == FAIL_NONE; dist += 400) {
            for(first=1; first<=3 && failReason == FAIL_NONE; first++) {
                for(align=0; align<4 && failReason == FAIL_NONE; align++) {
                    opSeek(fsize - dist, 0);
                    opRead(first, align);
                    opRead(2000, align);
                }
            }
        }
    }

    caseEnd(0x0604, "Big read near EOF, odd/even buffer");
}

// 0605 - tell + read around the read buffer edges
static void test0605(void)
{
    DWORD k;
    caseStart();

    if(createPatternFile(RND_FILESIZE, 4) && opOpen(0)) {
        for(k=509; k<=515 && failReason == FAIL_NONE; k++) {
            opSeek(0, 0);
            opRead(k, 0);
            opTell();
            opRead(1, 1);
            opSeek(-2, 1);
            opRead(3, 0);
            opTell();
            opSeek(k, 0);
            opRead(1100, 0);                    // big read (> 1024) starting at the buffer edge
            opTell();
        }
    }

    caseEnd(0x0605, "Tell + read at buffer edges");
}

// 0606 - write into area which is in the read buffer, read it back
static void test0606(void)
{
    caseStart();

    if(createPatternFile(RND_FILESIZE, 5) && opOpen(2)) {
        opRead(100, 0);                         // buffer now holds 0..511
        opSeek(200, 0);
        opWrite(50, 0, 0x33);                   // change 200..249
        opSeek(150, 0);
        opRead(200, 0);                         // must see the new data

        opSeek(1000, 0);
        opRead(100, 0);                         // buffer now 1000..1511
        opWrite(50, 1, 0x44);                   // write at 1100 without a seek
        opRead(100, 0);                         // continue reading at 1150
        opSeek(1050, 0);
        opRead(200, 1);                         // across the written part

        opSeek(3000, 0);
        opRead(10, 0);
        opSeek(20, 1);                          // SEEK_CUR inside buffer, then write
        opWrite(30, 0, 0x55);
        opSeek(-60, 1);
        opRead(100, 0);

        verifyFileAfterClose();
    }

    caseEnd(0x0606, "Write into read-buffered area");
}

// 0607 - write then read without seek, small and big
static void test0607(void)
{
    caseStart();

    if(createPatternFile(RND_FILESIZE, 6) && opOpen(2)) {
        opWrite(300, 0, 0x61);
        opRead(300, 0);
        opWrite(1, 1, 0x62);
        opRead(1, 1);
        opWrite(2000, 0, 0x63);
        opRead(2000, 1);
        opTell();
        opSeek(0, 0);
        opRead(5000, 0);
        verifyFileAfterClose();
    }

    caseEnd(0x0607, "Write then read without seek");
}

// 0608 - extend the file by writing at the end, read the new part back
static void test0608(void)
{
    caseStart();

    if(createPatternFile(RND_FILESIZE, 7) && opOpen(2)) {
        opSeek(-10, 2);
        opRead(5, 0);
        opSeek(0, 2);
        opWrite(1000, 0, 0x71);                 // file grows by 1000
        opSeek(-1000, 1);
        opRead(1000, 1);
        opSeek(0, 2);                           // must return the new size
        opSeek(-1500, 2);
        opRead(4000, 0);                        // returns 1500
        verifyFileAfterClose();
    }

    caseEnd(0x0608, "Extend file by write, read back");
}

//--------------------------------------------------
// edge cases - return values might differ between TOS versions. Values here are what TOS should return;
// if a native TOS drive shows something else, fix the table (expTos1 for TOS 1.x, expTos2plus for TOS 2.x and newer).
#define E_ERANGE   -64
#define E_EIHNDL   -37

#define EDGE_SEEK_SET_PAST_EOF      1
#define EDGE_POS_AFTER_BAD_SEEK     2
#define EDGE_SEEK_SET_NEGATIVE      3
#define EDGE_SEEK_END_PLUS          4
#define EDGE_SEEK_CUR_PAST_EOF      5
#define EDGE_SEEK_SET_TO_EOF        6
#define EDGE_FREAD_ZERO             7
#define EDGE_FREAD_AT_EOF           8
#define EDGE_FSEEK_CLOSED           9
#define EDGE_FREAD_CLOSED           10

typedef struct {
    WORD    testNo;
    WORD    which;
    char   *name;
    int32_t expTos1;
    int32_t expTos2plus;
} TEdgeCase;

#define EDGE_FILESIZE   1000

static const TEdgeCase edgeCases[] = {
    {0x0611, EDGE_SEEK_SET_PAST_EOF,  "Fseek SET past EOF (1001)",     E_ERANGE, E_ERANGE},
    {0x0612, EDGE_POS_AFTER_BAD_SEEK, "Position after failed Fseek",   100,      100},
    {0x0613, EDGE_SEEK_SET_NEGATIVE,  "Fseek SET -1",                  E_ERANGE, E_ERANGE},
    {0x0614, EDGE_SEEK_END_PLUS,      "Fseek END +1",                  E_ERANGE, E_ERANGE},
    {0x0615, EDGE_SEEK_CUR_PAST_EOF,  "Fseek CUR past EOF",            E_ERANGE, E_ERANGE},
    {0x0616, EDGE_SEEK_SET_TO_EOF,    "Fseek SET exactly to EOF",      EDGE_FILESIZE, EDGE_FILESIZE},
    {0x0617, EDGE_FREAD_ZERO,         "Fread 0 bytes",                 0,        0},
    {0x0618, EDGE_FREAD_AT_EOF,       "Fread at EOF",                  0,        0},
    {0x0619, EDGE_FSEEK_CLOSED,       "Fseek on closed handle",        E_EIHNDL, E_EIHNDL},
    {0x061A, EDGE_FREAD_CLOSED,       "Fread on closed handle",        E_EIHNDL, E_EIHNDL},
    {0, 0, 0, 0, 0}
};

static int32_t runEdgeCase(WORD which)
{
    int32_t res = 0;

    if(!opOpen(0)) {
        return -9999;
    }

    Fseek(100, fh, 0);                          // all cases start at position 100

    switch(which) {
        case EDGE_SEEK_SET_PAST_EOF:    res = Fseek(EDGE_FILESIZE + 1, fh, 0);  break;
        case EDGE_POS_AFTER_BAD_SEEK:   Fseek(EDGE_FILESIZE + 1, fh, 0);
                                        res = Fseek(0, fh, 1);                  break;
        case EDGE_SEEK_SET_NEGATIVE:    res = Fseek(-1, fh, 0);                 break;
        case EDGE_SEEK_END_PLUS:        res = Fseek(1, fh, 2);                  break;
        case EDGE_SEEK_CUR_PAST_EOF:    res = Fseek(EDGE_FILESIZE, fh, 1);      break;
        case EDGE_SEEK_SET_TO_EOF:      res = Fseek(EDGE_FILESIZE, fh, 0);      break;
        case EDGE_FREAD_ZERO:           res = Fread(fh, 0, ioBuf);              break;
        case EDGE_FREAD_AT_EOF:         Fseek(0, fh, 2);
                                        res = Fread(fh, 10, ioBuf);             break;
        case EDGE_FSEEK_CLOSED:         Fclose(fh);
                                        res = Fseek(0, fh, 0);
                                        fh = -1;                                break;
        case EDGE_FREAD_CLOSED:         Fclose(fh);
                                        res = Fread(fh, 10, ioBuf);
                                        fh = -1;                                break;
    }

    opClose();
    return res;
}

static void test061x(void)
{
    char tmp[HIST_LEN];
    int i;

    caseStart();
    if(!createPatternFile(EDGE_FILESIZE, 8)) {
        caseEnd(0x0610, "Edge cases - create file");
        return;
    }

    for(i=0; edgeCases[i].testNo != 0; i++) {
        const TEdgeCase *ec = &edgeCases[i];
        int32_t exp = ((tosVersion >> 8) < 2) ? ec->expTos1 : ec->expTos2plus;

        caseStart();
        opIndex = 1;
        int32_t res = runEdgeCase(ec->which);

        ce_snprintf(tmp, sizeof(tmp), "returned %ld, expected %ld (TOS %x)", res, exp, tosVersion);

        if(res != exp) {
            fail(FAIL_EDGE, tmp);
        }

        caseEnd(ec->testNo, ec->name);
    }
}

//--------------------------------------------------
// random sequences
static const DWORD rndReadSizes[] = {1, 2, 3, 4, 7, 14, 16, 255, 256, 257, 511, 512, 513, 1000, 1023, 1024, 1025, 1026,
                                     1100, 1536, 2000, 4000, 7090, 8192};
#define RND_READ_SIZES  (sizeof(rndReadSizes) / sizeof(rndReadSizes[0]))

static void randomOp(BYTE withWrites)
{
    uint32_t r = rnd() % 100;
    WORD align = rnd() & 3;
    int32_t t;

    if(withWrites && r >= 85) {                                 // 15% writes
        DWORD n = rndReadSizes[rnd() % RND_READ_SIZES];
        opWrite(n, align, (WORD) (rnd() & 0xff));
        return;
    }

    if(withWrites) {                                            // spread the rest over 0..84
        r = (r * 100) / 85;
    }

    if(r < 45) {                                                // read
        opRead(rndReadSizes[rnd() % RND_READ_SIZES], align);
    } else if(r < 60) {                                         // small SEEK_CUR - often inside the read buffer
        int32_t d = (int32_t) (rnd() % 1201) - 600;
        t = (int32_t) fpos + d;
        if(t < 0)               d = -(int32_t) fpos;
        if(t > (int32_t) fsize) d = (int32_t) (fsize - fpos);
        opSeek(d, 1);
    } else if(r < 70) {                                         // tell
        opTell();
    } else if(r < 85) {                                         // SEEK_SET anywhere
        opSeek(rnd() % (fsize + 1), 0);
    } else if(r < 93) {                                         // SEEK_END back
        opSeek(-(int32_t) (rnd() % (fsize + 1)), 2);
    } else {                                                    // SEEK_SET near EOF
        t = (int32_t) fsize - (int32_t) (rnd() % 1100);
        opSeek((t < 0) ? 0 : t, 0);
    }
}

static void testRandom(WORD testNo, char *name, uint32_t seed, DWORD opCount, BYTE withWrites, BYTE useTT)
{
    caseStart();
    rndState = seed;

    if(useTT) {
        if(!ttBuf) {
            out_tr_b(testNo, "TT RAM not available - skipped", 1);
            hostLog("T06 TT RAM test skipped - no TT RAM");
            return;
        }
        curBuf = ttBuf;
    }

    if(createPatternFile(RND_FILESIZE, (WORD) seed) && opOpen(withWrites ? 2 : 0)) {
        DWORD i;
        for(i=0; i<opCount && failReason == FAIL_NONE; i++) {
            randomOp(withWrites);

            if((i & 127) == 0) {                                // show that we're alive
                Cconout('.');
            }
        }
        (void) Cconws("\r");

        if(withWrites) {
            verifyFileAfterClose();
        }
    }

    caseEnd(testNo, name);
}

//--------------------------------------------------
static BYTE allocBuffers(void)
{
    shadow = (BYTE *) Malloc(SHADOW_SIZE);
    ioRaw  = (BYTE *) Malloc(IOBUF_SIZE);
    ttRaw  = 0;
    ttBuf  = 0;

    if(!shadow || !ioRaw) {
        return 0;
    }

    ioBuf = (BYTE *) ((((DWORD) ioRaw) + 3) & ~3);

    if((tosVersion >> 8) >= 3) {                                // Mxalloc only on newer TOS
        ttRaw = (BYTE *) Mxalloc(IOBUF_SIZE, 1);                // TT RAM only
        if(((int32_t) ttRaw) <= 0) {
            ttRaw = 0;
        } else {
            ttBuf = (BYTE *) ((((DWORD) ttRaw) + 3) & ~3);
        }
    }

    return 1;
}

static void freeBuffers(void)
{
    if(shadow) Mfree(shadow);
    if(ioRaw)  Mfree(ioRaw);
    if(ttRaw)  Mfree(ttRaw);
    shadow = ioRaw = ttRaw = 0;
}

void test06(void)
{
    out_s("Fread / Fseek / Fwrite through file buffers");
    out_s("err code: RRoooooo - RR: 1 open, 2 Fread ret, 3 data, 4 guard,");
    out_s("  5 Fseek ret, 6 Fwrite ret, 7 verify, 8 edge; oooooo: op index");

    Dsetdrv(drive);
    Dsetpath("\\");

    if(hostLogAvailable()) {
        out_s("Results are also sent to host log (core_hdd.log).");
        char tmp[64];
        ce_snprintf(tmp, sizeof(tmp), "T06 start - TOS %x, drive %c:", tosVersion, 'A' + drive);
        hostLog(tmp);
    }

    if(!allocBuffers()) {
        out_s("Failed to allocate buffers, terminating.");
        freeBuffers();
        return;
    }

    test0601();
    test0602();
    test0603();
    test0604();
    test0605();
    test0606();
    test0607();
    test0608();

    test061x();

    testRandom(0x0620, "Random read+seek, seed 1",         1, 1500, 0, 0);
    testRandom(0x0621, "Random read+seek, seed 2",         2, 1500, 0, 0);
    testRandom(0x0622, "Random read+seek, seed 3",     12345, 1500, 0, 0);
    testRandom(0x0623, "Random read+seek, TT RAM",     54321, 1500, 0, 1);

    testRandom(0x0630, "Random read+write+seek, seed 1",  11,  800, 1, 0);
    testRandom(0x0631, "Random read+write+seek, seed 2",  22,  800, 1, 0);

    Fdelete(T06_FILE);
    freeBuffers();

    hostLog("T06 done");
    out_s("");
}
