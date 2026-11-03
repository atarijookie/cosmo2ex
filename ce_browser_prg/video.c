// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <mint/osbind.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../ce_dd_prg/screencast.h"
#include "video.h"

// all the modes the browser knows - which of them this machine can show is in avail[]
static const VMode allModes[] = {
    { SCREENCAST_RES_ST_LOW,  0,  320, 200, 4, "ST low   320 x 200, 16 colours" },
    { SCREENCAST_RES_ST_MID,  1,  640, 200, 2, "ST mid   640 x 200, 4 colours" },
    { SCREENCAST_RES_ST_HIGH, 2,  640, 400, 1, "ST high  640 x 400, mono" },
    { SCREENCAST_RES_TT_LOW,  7,  320, 480, 8, "TT low   320 x 480, 256 colours" },
    { SCREENCAST_RES_TT_MID,  4,  640, 480, 4, "TT mid   640 x 480, 16 colours" },
    { SCREENCAST_RES_TT_HIGH, 6, 1280, 960, 1, "TT high 1280 x 960, mono" },
};
#define MODE_ST_LOW     0
#define MODE_ST_MID     1
#define MODE_ST_HIGH    2
#define MODE_TT_LOW     3
#define MODE_TT_MID     4
#define MODE_TT_HIGH    5

static const VMode *avail[6];
static int availCount;

const VMode *curMode;
uint8_t     *screen;
uint16_t     lineBytes;
uint8_t      fontH;
uint16_t     textCols;
uint8_t      machineType;

static uint8_t  *screenRaw;
static int16_t   oldRez;
static void     *oldPhys;
static void     *oldLog;
static uint16_t  oldPalST[16];
static uint16_t  oldPalTT[256];
static uint16_t  palST[16];         // Setpalette() reads it at the next VBL - must stay valid

// Line-A
static uint8_t  *la;                // variables base
static uint8_t **laFonts;           // 6x6, 8x8, 8x16 system font headers
static int16_t   oldLa[7];          // what videoSetMode() changes, restored at the end

#define LA_W(off)   (*((volatile int16_t *) (la + (off))))
#define LA_PLANES       0
#define LA_LIN_WR       2
#define LA_INTIN        8
#define LA_BYTES_LIN    (-2)
#define LA_REZ_VT       (-4)
#define LA_REZ_HZ       (-12)
#define LA_DEV_TAB0     (-692)
#define LA_DEV_TAB1     (-690)
#define LA_GCURX        (-602)
#define LA_GCURY        (-600)
#define LA_MOUSE_BT     (-596)

static void lineaInit(void)
{
    register uint8_t  *a0 __asm__("a0");
    register uint8_t **a1 __asm__("a1");
    __asm__ volatile (".dc.w 0xa000" : "=r"(a0), "=r"(a1) : : "d0", "d1", "d2", "a2", "cc", "memory");
    la      = a0;
    laFonts = a1;
}

void mouseHide(void)
{
    __asm__ volatile (".dc.w 0xa00a" : : : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

static void mouseShowForce(uint8_t force)
{
    int16_t *intin = *((int16_t **) (la + LA_INTIN));
    intin[0] = force ? 0 : 1;           // 0 = show no matter how many hides were before, else undo one hide
    __asm__ volatile (".dc.w 0xa009" : : : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

void mouseShow(void)
{
    mouseShowForce(FALSE);
}

// the standard arrow - GEM leaves the busy bee there while it starts a program
static int16_t arrowForm[37] = {
    0, 0,                       // hot spot x, y
    1, 0, 1,                    // planes, mask colour (white), data colour (black)
    // mask
    0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
    0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380,
    // data
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000
};

// scroll pointers - two drawn by hand, the rest flipped / turned from them; the white mask is the black shape grown by 1 pixel
static const uint16_t shapeUp[16] = {
    0x0100, 0x0380, 0x07c0, 0x0fe0, 0x1ff0, 0x3ff8, 0x0380, 0x0380,
    0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x0000, 0x0000
};
static const uint16_t shapeUpLeft[16] = {
    0xfc00, 0xf800, 0xf000, 0xf800, 0xdc00, 0x8e00, 0x0700, 0x0380,
    0x01c0, 0x00e0, 0x0070, 0x0020, 0x0000, 0x0000, 0x0000, 0x0000
};
static int16_t scrollForms[9][37];      // index (dy + 1) * 3 + (dx + 1); [4] is unused (that's the arrow)
static int8_t  curPointer = -1;

static void flipH(uint16_t *s)          // mirror left - right
{
    int r, b;
    for(r=0; r<16; r++) {
        uint16_t v = 0;
        for(b=0; b<16; b++) {
            if(s[r] & (1 << b)) {
                v |= 0x8000 >> b;
            }
        }
        s[r] = v;
    }
}

static void flipV(uint16_t *s)          // mirror top - bottom
{
    int r;
    for(r=0; r<8; r++) {
        uint16_t t = s[r]; s[r] = s[15 - r]; s[15 - r] = t;
    }
}

static void transpose(uint16_t *s)      // swap rows and columns - up becomes left
{
    uint16_t t[16];
    int r, c;
    for(r=0; r<16; r++) {
        t[r] = 0;
        for(c=0; c<16; c++) {
            if(s[c] & (0x8000 >> r)) {
                t[r] |= 0x8000 >> c;
            }
        }
    }
    for(r=0; r<16; r++) {
        s[r] = t[r];
    }
}

static void makeForm(int16_t *form, const uint16_t *shape)
{
    int r;
    form[0] = 7; form[1] = 7;           // hot spot in the middle - clicks land where they would with the arrow
    form[2] = 1; form[3] = 0; form[4] = 1;
    for(r=0; r<16; r++) {
        uint16_t m = 0;
        int k;
        for(k=r-1; k<=r+1; k++) {
            if(k >= 0 && k < 16) {
                m |= shape[k] | (shape[k] << 1) | (shape[k] >> 1);
            }
        }
        form[5 + r]  = (int16_t) m;
        form[21 + r] = (int16_t) shape[r];
    }
}

static void makeScrollForms(void)
{
    uint16_t s[16];
    int i;

    #define MAKE(idx, base, fh, fv, tr)  do { for(i=0; i<16; i++) s[i] = base[i]; \
            if(tr) transpose(s); if(fh) flipH(s); if(fv) flipV(s); makeForm(scrollForms[idx], s); } while(0)

    MAKE(1, shapeUp,     0, 0, 0);      // up
    MAKE(7, shapeUp,     0, 1, 0);      // down
    MAKE(3, shapeUp,     0, 0, 1);      // left
    MAKE(5, shapeUp,     1, 0, 1);      // right
    MAKE(0, shapeUpLeft, 0, 0, 0);      // up left
    MAKE(2, shapeUpLeft, 1, 0, 0);      // up right
    MAKE(6, shapeUpLeft, 0, 1, 0);      // down left
    MAKE(8, shapeUpLeft, 1, 1, 0);      // down right
    #undef MAKE
}

static void setForm(int16_t *form)
{
    int16_t **pIntin = (int16_t **) (la + LA_INTIN);
    int16_t *oldIntin = *pIntin;

    *pIntin = form;             // Line-A 'transform mouse' takes the form from INTIN
    __asm__ volatile (".dc.w 0xa00b" : : : "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
    *pIntin = oldIntin;
}

void mousePointer(int8_t dx, int8_t dy)
{
    int8_t idx = (dy + 1) * 3 + (dx + 1);
    if(idx == curPointer) {
        return;
    }
    curPointer = idx;
    setForm(idx == 4 ? arrowForm : scrollForms[idx]);
}

void mouseGet(int16_t *x, int16_t *y, uint8_t *buttons)
{
    *x       = LA_W(LA_GCURX);
    *y       = LA_W(LA_GCURY);
    *buttons = LA_W(LA_MOUSE_BT) & 3;
}

//--------------------------------------------------
static uint32_t cookieId, cookieValue;

static void readCookieSuper(void)
{
    uint32_t *jar = *((uint32_t **) 0x5a0);

    cookieValue = 0;
    for(; jar && jar[0] != 0; jar += 2) {
        if(jar[0] == cookieId) {
            cookieValue = jar[1];
            return;
        }
    }
}

static uint32_t readCookie(uint32_t id)
{
    cookieId = id;
    Supexec(readCookieSuper);
    return cookieValue;
}

uint8_t videoDetect(const char **error)
{
    int i;

    machineType = (uint8_t) (readCookie(0x5f4d4348UL) >> 16);     // '_MCH': 0 ST, 1 STE, 2 TT, 3 Falcon
    oldRez = Getrez();
    availCount = 0;

    // A colour monitor can't show ST high and the mono monitor shows only ST high (TT: VGA shows all but TT high,
    // the TT high monitor only TT high) - the mode the machine is in now tells which monitor it has.
    switch(machineType) {
        case MACHINE_TYPE_TT:
            if(oldRez == allModes[MODE_TT_HIGH].xbiosRez) {
                avail[availCount++] = &allModes[MODE_TT_HIGH];
            } else {
                for(i=MODE_ST_LOW; i<=MODE_TT_MID; i++) {
                    avail[availCount++] = &allModes[i];
                }
            }
            break;

        case MACHINE_TYPE_FALCON:                                   // only its ST compatible modes, and no switching yet
            if(oldRez >= 0 && oldRez <= 2) {
                avail[availCount++] = &allModes[oldRez];
            }
            break;

        default:                                                    // ST, STE, Mega ST(E)
            if(oldRez == 2) {
                avail[availCount++] = &allModes[MODE_ST_HIGH];
            } else {
                avail[availCount++] = &allModes[MODE_ST_LOW];
                avail[availCount++] = &allModes[MODE_ST_MID];
            }
            break;
    }

    if(availCount == 0) {
        *error = "This video mode is not supported - on Falcon use an ST compatible mode.\r\n";
        return FALSE;
    }
    return TRUE;
}

int videoModeCount(void)
{
    return availCount;
}

const VMode *videoModeAt(int i)
{
    return avail[i];
}

//--------------------------------------------------
uint8_t videoStart(void)
{
    uint32_t size = 0;
    int i;

    for(i=0; i<availCount; i++) {                                   // memory for the biggest mode we may switch to
        uint32_t s = (uint32_t) avail[i]->width * avail[i]->height * avail[i]->planes / 8;
        if(s > size) {
            size = s;
        }
    }
    size += 256;

    if(machineType >= MACHINE_TYPE_TT) {
        screenRaw = (uint8_t *) Mxalloc(size, 0);                   // ST RAM - video can't show TT RAM
    } else {
        screenRaw = (uint8_t *) Malloc(size);                       // old TOS has no Mxalloc, and only ST RAM anyway
    }
    if(!screenRaw || ((int32_t) screenRaw) < 0) {
        screenRaw = NULL;
        return FALSE;
    }
    screen = (uint8_t *) ((((uint32_t) screenRaw) + 255) & 0xffffff00);    // ST shifter needs 256 B aligned screen

    oldPhys = Physbase();
    oldLog  = Logbase();
    for(i=0; i<16; i++) {
        oldPalST[i] = Setcolor(i, -1);
    }
    if(machineType == MACHINE_TYPE_TT) {
        EgetPalette(0, 256, oldPalTT);
    }

    lineaInit();
    oldLa[0] = LA_W(LA_PLANES);
    oldLa[1] = LA_W(LA_LIN_WR);
    oldLa[2] = LA_W(LA_BYTES_LIN);
    oldLa[3] = LA_W(LA_REZ_VT);
    oldLa[4] = LA_W(LA_REZ_HZ);
    oldLa[5] = LA_W(LA_DEV_TAB0);
    oldLa[6] = LA_W(LA_DEV_TAB1);

    const VMode *m = avail[0];
    for(i=0; i<availCount; i++) {                                   // start in the mode the machine is in
        if(avail[i]->xbiosRez == oldRez) {
            m = avail[i];
        }
    }
    uint8_t ok = videoSetMode(m);
    makeScrollForms();
    curPointer = -1;
    mousePointer(0, 0);                                             // arrow instead of GEM's busy bee
    return ok;
}

uint8_t videoSetMode(const VMode *m)
{
    mouseHide();

    if(m->xbiosRez == Getrez()) {
        Setscreen(screen, screen, -1);                              // same mode, just our screen memory
    } else {
        Setscreen(screen, screen, m->xbiosRez);
    }
    Vsync();

    curMode   = m;
    lineBytes = (uint16_t) ((uint32_t) m->width * m->planes / 8);
    fontH     = (m->height < 400) ? 8 : 16;
    textCols  = m->width / 8;

    // the mouse pointer and its limits come from these - TOS doesn't update all of them on Setscreen()
    LA_W(LA_PLANES)    = m->planes;
    LA_W(LA_LIN_WR)    = lineBytes;
    LA_W(LA_BYTES_LIN) = lineBytes;
    LA_W(LA_REZ_VT)    = m->height;
    LA_W(LA_REZ_HZ)    = m->width;
    LA_W(LA_DEV_TAB0)  = m->width  - 1;
    LA_W(LA_DEV_TAB1)  = m->height - 1;
    LA_W(LA_GCURX)     = m->width  / 2;
    LA_W(LA_GCURY)     = m->height / 2;

    fillRows(0, m->height, 0);

    if(m->planes > 1) {                                             // colour 0 white, 1 black for the toolbar and menus
        uint16_t ui[2];
        if(m->res & 0x10) {                                         // TT mode - 4 bits per channel
            ui[0] = 0x0fff; ui[1] = 0x0000;
            EsetPalette(0, 2, ui);
        } else {
            palST[0] = 0x0777; palST[1] = 0x0000;
            (void) Setcolor(0, palST[0]);
            (void) Setcolor(1, palST[1]);
        }
    }

    mouseShowForce(TRUE);
    return TRUE;
}

void videoEnd(void)
{
    mouseHide();

    uint8_t rezChanged = (Getrez() != oldRez);
    Setscreen(oldLog, oldPhys, rezChanged ? oldRez : -1);           // with a mode change TOS clears that screen
    Vsync();

    LA_W(LA_PLANES)    = oldLa[0];
    LA_W(LA_LIN_WR)    = oldLa[1];
    LA_W(LA_BYTES_LIN) = oldLa[2];
    LA_W(LA_REZ_VT)    = oldLa[3];
    LA_W(LA_REZ_HZ)    = oldLa[4];
    LA_W(LA_DEV_TAB0)  = oldLa[5];
    LA_W(LA_DEV_TAB1)  = oldLa[6];

    if(machineType == MACHINE_TYPE_TT) {
        EsetPalette(0, 256, oldPalTT);
    } else {
        Setpalette(oldPalST);
        Vsync();                                                    // Setpalette() takes it at the VBL
    }

    mouseShowForce(TRUE);

    if(screenRaw) {
        Mfree(screenRaw);
        screenRaw = NULL;
    }
}

//--------------------------------------------------
// drawing

static void copyLongs(uint8_t *dst, const uint8_t *src, uint32_t bytes)    // both even
{
    uint32_t *d = (uint32_t *) dst;
    const uint32_t *s = (const uint32_t *) src;
    uint32_t n = bytes >> 4;

    while(n--) {                                                    // 16 B per round - 4x faster than byte memcpy()
        *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
    }
    n = (bytes & 15) >> 1;
    uint16_t *dw = (uint16_t *) d;
    const uint16_t *sw = (const uint16_t *) s;
    while(n--) {
        *dw++ = *sw++;
    }
}

void copyRect(uint16_t x16, uint16_t y, uint16_t w16, uint16_t h, const uint8_t *src)
{
    uint32_t rowBytes = (uint32_t) w16 * curMode->planes * 2;
    uint8_t *dst = screen + (uint32_t) y * lineBytes + (uint32_t) x16 * curMode->planes * 2;

    if(rowBytes == lineBytes) {                                     // whole width - one block
        copyLongs(dst, src, rowBytes * h);
        return;
    }
    while(h--) {
        copyLongs(dst, src, rowBytes);
        dst += lineBytes;
        src += rowBytes;
    }
}

void fillRows(uint16_t y, uint16_t rows, uint8_t colour)
{
    uint8_t  planes = curMode->planes;
    uint16_t *d = (uint16_t *) (screen + (uint32_t) y * lineBytes);
    uint32_t groups = (uint32_t) rows * lineBytes / 2 / planes;     // 16 pixel groups
    uint16_t pat[8];
    int p;

    for(p=0; p<planes; p++) {
        pat[p] = ((colour >> p) & 1) ? 0xffff : 0x0000;
    }

    if(planes == 1) {
        uint32_t *dl = (uint32_t *) d;
        uint32_t v = pat[0] ? 0xffffffffUL : 0;
        uint32_t n = groups / 2;
        while(n--) {
            *dl++ = v;
        }
        return;
    }

    while(groups--) {
        for(p=0; p<planes; p++) {
            *d++ = pat[p];
        }
    }
}

void drawText(uint16_t col, uint16_t y, const char *s, uint16_t maxCols, uint8_t inverse)
{
    uint8_t *font = laFonts[fontH == 8 ? 1 : 2];
    uint16_t first   = *((uint16_t *) (font + 36));
    uint16_t last    = *((uint16_t *) (font + 38));
    uint16_t *offTab = *((uint16_t **) (font + 72));
    uint8_t  *data   = *((uint8_t **)  (font + 76));
    uint16_t formW   = *((uint16_t *) (font + 80));
    uint8_t  planes  = curMode->planes;
    uint8_t  flip    = inverse ? 0xff : 0x00;
    uint16_t i;
    uint8_t  r, p;

    for(i=0; i<maxCols && col < textCols; i++, col++) {
        uint8_t c = *s ? (uint8_t) *s++ : ' ';
        if(c < first || c > last) {
            c = '?';
        }
        const uint8_t *glyph = data + (offTab[c - first] >> 3);
        uint8_t *dst = screen + (uint32_t) y * lineBytes + (col >> 1) * planes * 2 + (col & 1);

        for(r=0; r<fontH; r++) {
            dst[0] = glyph[0] ^ flip;                               // colour 1 (black) where the glyph is, 0 elsewhere
            for(p=1; p<planes; p++) {
                dst[p * 2] = 0;
            }
            glyph += formW;
            dst   += lineBytes;
        }
    }
}

void setPalette(const uint16_t *pal, uint16_t count)
{
    uint16_t i;

    if(curMode->planes == 1) {
        return;                                                     // mono keeps white background, black pixels
    }

    if(curMode->res & 0x10) {                                       // TT mode
        EsetPalette(0, count, (void *) pal);
        return;
    }

    for(i=0; i<16; i++) {
        palST[i] = (i < count) ? pal[i] : 0;
    }
    Setpalette(palST);
}
