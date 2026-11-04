// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Audio player screen:
//
//    CosmosEx audio player                      (inverse)
//
//   File: C:\MUSIC\SMOOTH.MP3                  (too long: ...\SMOOTH.MP3)
//   Title: Smooth Operator                      (when the file has a title tag)
//
//   [ << ] [ || ] [ >> ]   01:23 / 04:56
//
//   [==========>-------------------------]
//   playing                                      (paused / buffering 60%)
//
//   Sound: YM chip, 15754 Hz, buffer 1000 ms
//
//   Esc        quit
//   Space      pause / play
//   <- ->      back / forward 10 s
//   Down Up    back / forward 1 min
//
// Keyboard only - no mouse: its pointer and packets cost a plain ST enough to slow the sound down. Text through the
// VT52 console (Cconws), screen size from Line-A. Only what changed is drawn again.

#include <mint/osbind.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "med.h"
#include "player.h"
#include "ui.h"

#define ROW_HEADER      0
#define ROW_NAME        2
#define ROW_TITLE       3
#define ROW_BUTTONS     5
#define ROW_BAR         7
#define ROW_STATE       8
#define ROW_SOUND       10
#define ROW_HELP        12

#define COL_BACK        1           // "[ << ]"
#define COL_PAUSE       8           // "[ || ]"
#define COL_FORWARD     15          // "[ >> ]"
#define COL_TIME        23

// Line-A
static uint8_t *la;
#define LA_W(off)       (*((volatile int16_t *) (la + (off))))
#define LA_V_CEL_HT     (-46)
#define LA_V_CEL_MX     (-44)
#define LA_V_CEL_MY     (-42)

static int16_t cols, rows, cellH, barW;

// what's on the screen now - drawn again when it differs
static int32_t lastSec = -1;
static int16_t lastBar = -1;
static int8_t  lastPaused = -1;
static char    lastState[24];

//--------------------------------------------------------------------------------------------------------------------

static void lineaInit(void)
{
    register uint8_t *a0 __asm__("a0");
    __asm__ volatile (".dc.w 0xa000" : "=r"(a0) : : "d0", "d1", "d2", "a1", "a2", "cc", "memory");
    la = a0;
}

static void print(const char *s)
{
    (void) Cconws(s);
}

static void glyph(char c)               // any character of the font, also 1..31 (the arrows are 1..4)
{
    Bconout(5, c);
}

static void gotoxy(int16_t col, int16_t row)
{
    char s[5] = { 27, 'Y', (char) (32 + row), (char) (32 + col), 0 };
    print(s);
}

static void appendNum(char *out, uint32_t v, int minDigits)
{
    char buf[12];
    int  i = sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
        minDigits--;
    } while(v > 0 || minDigits > 0);
    strcat(out, buf + i);
}

static void appendTime(char *out, uint32_t ms)
{
    uint32_t s = ms / 1000;
    appendNum(out, s / 60, 2);
    strcat(out, ":");
    appendNum(out, s % 60, 2);
}

// s cut to fit from col to the right edge
static void printFit(int16_t col, const char *s)
{
    char buf[84];
    int16_t max = cols - col - 1;
    if(max > (int16_t) sizeof(buf) - 1) {
        max = sizeof(buf) - 1;
    }
    strncpy(buf, s, max);
    buf[max] = 0;
    print(buf);
}

static uint8_t sameText(const char *a, const char *b)  // ignoring case
{
    for(; *a && *b; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        char y = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if(x != y) {
            return FALSE;
        }
    }
    return *a == *b;
}

//--------------------------------------------------------------------------------------------------------------------

static void drawPauseButton(uint8_t paused)
{
    gotoxy(COL_PAUSE, ROW_BUTTONS);
    print(paused ? "[ >  ]" : "[ || ]");
}

static void drawHelpLine(int16_t row, const char *keys, const char *what)
{
    gotoxy(1, row);
    print(keys);
    gotoxy(12, row);
    printFit(12, what);
}

void uiStart(const char *path)
{
    const char *name = path;
    const char *p;
    char line[84];
    uint8_t afmt = playerAudioFormat();

    lineaInit();
    cols  = LA_W(LA_V_CEL_MX) + 1;
    rows  = LA_W(LA_V_CEL_MY) + 1;
    cellH = LA_W(LA_V_CEL_HT);
    if(cols < 40 || cols > 160 || cellH < 8 || cellH > 16) {        // Line-A said nonsense - assume ST low
        cols = 40; rows = 25; cellH = 8;
    }
    barW = cols - 4;
    if(barW > 70) {
        barW = 70;
    }

    for(p = path; *p; p++) {                                        // the file name, without the folders
        if(*p == '\\' || *p == ':') {
            name = p + 1;
        }
    }

    print("\33E\33f");                                              // clear, cursor off

    gotoxy(0, ROW_HEADER);                                          // header bar, inverse, whole width
    print("\33p");
    memset(line, ' ', cols);
    line[cols - 1] = 0;
    memcpy(line + 1, "CosmosEx audio player", 21);
    print(line);
    print("\33q");

    gotoxy(1, ROW_NAME);                                            // the path - when too long, its end with the name
    print("File: ");
    {
        int16_t avail = cols - 8;
        int16_t len   = strlen(path);
        if(len > avail) {
            print("...");
            print(path + len - (avail - 3));
        } else {
            print(path);
        }
    }
    if(playerTitle[0] && !sameText(playerTitle, name)) {
        gotoxy(1, ROW_TITLE);
        strcpy(line, "Title: ");
        strncpy(line + 7, playerTitle, sizeof(line) - 8);
        line[sizeof(line) - 1] = 0;
        printFit(1, line);
    }

    gotoxy(COL_BACK, ROW_BUTTONS);
    print("[ << ]");
    drawPauseButton(FALSE);
    gotoxy(COL_FORWARD, ROW_BUTTONS);
    print("[ >> ]");

    if(playerDurationMs()) {
        gotoxy(1, ROW_BAR);
        print("[");
        gotoxy(2 + barW, ROW_BAR);
        print("]");
    }

    gotoxy(1, ROW_SOUND);
    line[0] = 0;
    strcat(line, "Sound: ");
    strcat(line, afmt == MED_AFMT_YM ? "YM chip, " : afmt == MED_AFMT_DMA_S8_MONO ? "DMA, " : "none");
    if(afmt != MED_AFMT_NONE) {
        appendNum(line, playerClockRate(), 1);
        strcat(line, " Hz, ");
    } else {
        strcat(line, ", ");
    }
    strcat(line, "buffer ");
    appendNum(line, playerStats.latencyMs, 1);
    strcat(line, " ms");
    printFit(1, line);

    drawHelpLine(ROW_HELP,     "Esc",   "quit");
    drawHelpLine(ROW_HELP + 1, "Space", "pause / play");
    gotoxy(1, ROW_HELP + 2);   glyph(4); print("  "); glyph(3);      // left, right arrow
    gotoxy(12, ROW_HELP + 2);  printFit(12, "back / forward 10 s");
    gotoxy(1, ROW_HELP + 3);   glyph(2); print("  "); glyph(1);      // down, up arrow
    gotoxy(12, ROW_HELP + 3);  printFit(12, "back / forward 1 min");

    lastSec = -1;
    lastBar = -1;
    lastPaused = -1;
    lastState[0] = 0;
    uiUpdate();
}

void uiUpdate(void)
{
    uint32_t pos = playerPositionMs();
    uint32_t dur = playerDurationMs();
    int32_t  sec = pos / 1000;
    int16_t  bar = -1;
    uint8_t  paused = playerPaused();
    char     state[24];
    char     line[84];

    if(dur) {
        uint32_t p = (pos >= dur) ? dur : pos;
        bar = (int16_t) ((p / 100) * barW / (dur / 100 + 1));       // in 0.1 s - no overflow for hours of sound
    }

    state[0] = 0;
    if(paused) {
        strcat(state, "paused");
    } else if(playerBuffering()) {
        strcat(state, "buffering ");
        appendNum(state, playerBufferedPercent() / 10 * 10, 1);     // 10 % steps
        strcat(state, "%");
    } else {
        strcat(state, "playing");
    }

    if(sec != lastSec) {
        lastSec = sec;
        line[0] = 0;
        appendTime(line, pos);
        if(dur) {
            strcat(line, " / ");
            appendTime(line, dur);
        }
        gotoxy(COL_TIME, ROW_BUTTONS);
        printFit(COL_TIME, line);
        print("\33K");
    }

    if(bar != lastBar && dur) {
        int16_t i;
        lastBar = bar;
        for(i=0; i<barW; i++) {
            line[i] = (i < bar) ? '=' : (i == bar) ? '>' : '-';
        }
        line[barW] = 0;
        gotoxy(2, ROW_BAR);
        print(line);
    }

    if(paused != lastPaused) {
        lastPaused = paused;
        drawPauseButton(paused);
    }

    if(strcmp(state, lastState) != 0) {
        strcpy(lastState, state);
        gotoxy(1, ROW_STATE);
        print(state);
        print("\33K");
    }

}

void uiEnd(void)
{
    print("\33E\33e");                                              // clear, cursor on
}
