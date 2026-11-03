#ifndef _VIDEO_H_
#define _VIDEO_H_

// Screen of CE_BROWS.PRG: own screen memory (the desktop's screen stays untouched), video mode switching through
// XBIOS Setscreen(), the mouse pointer of Line-A, text with the system font straight to the bitplanes.
// Colour 0 is white and colour 1 black in every mode - ce_browser keeps them so in its palettes too.

#include <stdint.h>

typedef struct {
    uint8_t     res;            // SCREENCAST_RES_* - how ce_cores / ce_browser name it
    int8_t      xbiosRez;       // Getrez() / Setscreen() value
    uint16_t    width;
    uint16_t    height;
    uint8_t     planes;
    const char *name;
} VMode;

#define MACHINE_TYPE_ST     0
#define MACHINE_TYPE_STE    1
#define MACHINE_TYPE_TT     2
#define MACHINE_TYPE_FALCON 3

extern const VMode *curMode;
extern uint8_t     *screen;         // start of the screen memory in use
extern uint16_t     lineBytes;
extern uint8_t      fontH;          // 8 or 16
extern uint16_t     textCols;
extern uint8_t      machineType;

// modes this machine with this monitor can show; returns FALSE when it can't run the browser (Falcon in its own modes)
uint8_t  videoDetect(const char **error);
int      videoModeCount(void);
const VMode *videoModeAt(int i);

uint8_t  videoStart(void);                      // take over the screen in the current mode; FALSE = not enough memory
uint8_t  videoSetMode(const VMode *m);          // switch to another of the detected modes
void     videoEnd(void);                        // back to the desktop's screen and mode

void     mouseHide(void);                       // around every write to the screen - the pointer restores what was under it
void     mouseShow(void);
void     mouseGet(int16_t *x, int16_t *y, uint8_t *buttons);
void     mousePointer(int8_t dx, int8_t dy);    // the arrow (0, 0), or a scroll arrow pointing that way (-1 / 0 / 1 each)

void     fillRows(uint16_t y, uint16_t rows, uint8_t colour);
void     drawText(uint16_t col, uint16_t y, const char *s, uint16_t maxCols, uint8_t inverse);  // pads with spaces to maxCols
void     copyRect(uint16_t x16, uint16_t y, uint16_t w16, uint16_t h, const uint8_t *src);    // bitplane rows to the screen
void     setPalette(const uint16_t *pal, uint16_t count);  // colour modes only, mono is left as it is

#endif
