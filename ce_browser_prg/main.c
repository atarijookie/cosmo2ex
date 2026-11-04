// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// CE_BROWS.PRG - web browser for CosmosEx. The page is rendered on the host (ce_browser, headless Chromium) and comes
// as Atari bitplanes over ACSI / SCSI (HOSTMOD_BROWSER); this program only copies them to the screen, draws the
// toolbar and menus, and sends the mouse and keys back. No GEM - whole screen, written directly.

#include <mint/osbind.h>
#include <stdint.h>
#include <stddef.h>

#include "../libacsiscsi/global.h"
#include "../libacsiscsi/stdlib.h"
#include "video.h"
#include "brw.h"

// scancodes
#define SC_ESC      0x01
#define SC_BACKSP   0x0e
#define SC_TAB      0x0f
#define SC_RETURN   0x1c
#define SC_Z        0x2c
#define SC_F1       0x3b
#define SC_F2       0x3c
#define SC_F3       0x3d
#define SC_F4       0x3e
#define SC_F5       0x3f
#define SC_F10      0x44
#define SC_HOME     0x47
#define SC_UP       0x48
#define SC_LEFT     0x4b
#define SC_RIGHT    0x4d
#define SC_DOWN     0x50
#define SC_DELETE   0x53
#define SC_UNDO     0x61
#define SC_HELP     0x62
#define SC_ENTER    0x72

#define KB_SHIFT    0x03            // Kbshift() bits
#define KB_CTRL     0x04
#define KB_ALT      0x08

#define CTRL_D      0x04
#define CTRL_L      0x0c
#define CTRL_Q      0x11

#define TICKS_PER_S 200

typedef struct {
    uint8_t scan, ascii, shift;
} Key;

typedef struct {
    uint8_t  flags;
    uint8_t  progress;
    uint32_t latestSeq;
    char     url[BRW_STATUS_URL_SIZE];
    char     title[BRW_STATUS_TITLE_SIZE];
    char     msg[BRW_STATUS_MSG_SIZE];
} Status;

static Status   st;
static uint16_t toolH, pageTop, pageH;
static uint8_t  zoomQ = 4;
static uint8_t  modeSent;           // SET_MODE sent since the last mode / zoom change
static uint32_t baseSeq;            // frame completely on the screen (0 = none - next update is the whole page)
static uint16_t resumeRow;          // next row of a frame which comes in parts
static uint8_t  quit;
static char     localMsg[80];       // shown in the toolbar instead of the host's message (device not answering, ...)
static uint8_t  waitingForPage;     // page area is empty until the first picture comes - status shown there big
static uint8_t  frameSinceScroll = TRUE;    // a frame came after the last edge scroll jump - the next one may go

// URL history from the host
static char     histData[32 * 1024];
static char    *hist[BRW_HISTORY_MAX];
static int      histCount;

//--------------------------------------------------
// small helpers

static uint8_t readKey(Key *k)
{
    if(!Bconstat(2)) {
        return FALSE;
    }
    uint32_t v = Bconin(2);
    k->scan  = (v >> 16) & 0xff;
    k->ascii = v & 0xff;
    k->shift = Kbshift(-1) & 0x0f;
    return TRUE;
}

static uint8_t isQuitKey(const Key *k)
{
    return k->scan == SC_F10 || k->ascii == CTRL_Q || k->ascii == CTRL_D;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static uint8_t containsNoCase(const char *hay, const char *needle)
{
    int n = strlen(needle);
    if(n == 0) {
        return TRUE;
    }
    for(; *hay; hay++) {
        int i;
        for(i=0; i<n && hay[i] && lower(hay[i]) == lower(needle[i]); i++);
        if(i == n) {
            return TRUE;
        }
    }
    return FALSE;
}

static uint32_t ticks(void)
{
    return getTicksAsUser();
}

static void layout(void)
{
    toolH   = fontH + 2;
    pageTop = toolH;
    pageH   = curMode->height - toolH;
}

static uint16_t textRows(void)                      // text rows in the page area
{
    return pageH / fontH;
}

// a line of text in the page area, row counted from its top
static void pageText(uint16_t row, uint16_t col, const char *s, uint8_t inverse)
{
    if(row < textRows()) {
        drawText(col, pageTop + row * fontH, s, textCols - col, inverse);
    }
}

static void clearPage(void)
{
    fillRows(pageTop, pageH, 0);
}

static void ensureMode(void)
{
    if(!modeSent && brwSetMode(curMode->res, pageH, zoomQ) == BRW_OK) {
        modeSent = TRUE;
    }
}

// the page area was drawn over - ask for the whole page again
static void needWholePage(void)
{
    baseSeq   = 0;
    resumeRow = 0;
}

//--------------------------------------------------
// toolbar

static void drawToolbar(void)
{
    char line[200];
    const char *right;
    const char *text;
    int cols = textCols;
    int i, len, rlen;

    if(localMsg[0]) {
        text = localMsg;
    } else if(st.msg[0]) {
        text = st.msg;
    } else if(st.title[0]) {
        text = st.title;
    } else if(st.url[0]) {
        text = st.url;
    } else {
        text = "Ctrl+L or F3: open URL";
    }

    if(!(st.flags & BRW_STF_CONNECTED)) {
        right = "";
    } else if(st.flags & BRW_STF_LOADING) {
        right = "Loading";
    } else {
        right = (zoomQ == 4) ? "F1=Help" : "2x F1=Help";
    }

    for(i=0; i<cols && i<(int) sizeof(line) - 1; i++) {
        line[i] = ' ';
    }
    line[i] = 0;

    line[0] = (st.flags & BRW_STF_CAN_BACK)    ? '<' : ' ';
    line[1] = (st.flags & BRW_STF_CAN_FORWARD) ? '>' : ' ';

    rlen = strlen(right);
    len  = strlen(text);
    if(len > cols - 4 - rlen) {
        len = cols - 4 - rlen;
    }
    for(i=0; i<len; i++) {
        line[3 + i] = text[i];
    }
    for(i=0; i<rlen; i++) {
        line[cols - rlen + i] = right[i];
    }

    mouseHide();
    fillRows(0, 1, 0);
    drawText(0, 1, line, cols, FALSE);
    fillRows(toolH - 1, 1, 1);
    mouseShow();
}

static void readStatus(void)
{
    if(brwGetStatus() != BRW_OK) {
        strcpy(localMsg, "Cosmo2ex is not answering");
        return;
    }
    localMsg[0] = 0;
    st.flags     = dmaBuf[0];
    st.progress  = dmaBuf[1];
    st.latestSeq = getDwordBE(dmaBuf + 8);
    strncpy(st.url,   (char *) dmaBuf + BRW_STATUS_URL_OFFSET,   BRW_STATUS_URL_SIZE);
    strncpy(st.title, (char *) dmaBuf + BRW_STATUS_TITLE_OFFSET, BRW_STATUS_TITLE_SIZE);
    strncpy(st.msg,   (char *) dmaBuf + BRW_STATUS_MSG_OFFSET,   BRW_STATUS_MSG_SIZE);
    st.url[BRW_STATUS_URL_SIZE - 1] = st.title[BRW_STATUS_TITLE_SIZE - 1] = st.msg[BRW_STATUS_MSG_SIZE - 1] = 0;
}

// empty page area while the page is being prepared: what is going on, in the middle of it
static void showWaitText(void)
{
    const char *m = localMsg[0] ? localMsg : (st.msg[0] ? st.msg : "Loading...");
    int len = strlen(m);
    int col = (len < (int) textCols) ? ((int) textCols - len) / 2 : 0;
    uint16_t y = pageTop + (pageH / 2) - fontH;

    mouseHide();
    fillRows(y, fontH * 2, 0);
    drawText(col, y + fontH / 2, m, textCols - col, FALSE);
    mouseShow();
}

// page area cleared - wait for the page with the status in it
static void startWaiting(void)
{
    mouseHide();
    clearPage();
    mouseShow();
    waitingForPage = TRUE;
    showWaitText();
}

// status from the host; toolbar redrawn when something in it changed
static void updateStatus(void)
{
    static Status shown;
    static char shownLocal[80];

    readStatus();
    if(memcmp(&shown, &st, sizeof(st)) != 0 || strcmp(shownLocal, localMsg) != 0) {
        memcpy(&shown, &st, sizeof(st));
        strcpy(shownLocal, localMsg);
        drawToolbar();
        if(waitingForPage) {
            showWaitText();
        }
    }
}

//--------------------------------------------------
// page updates

// gets what changed on the page and draws it; TRUE if something was drawn
static uint8_t pageUpdate(void)
{
    uint8_t drawn = FALSE;
    int parts;

    for(parts = 0; parts < 4; parts++) {                    // a TT frame comes in 2 parts, don't loop forever
        uint8_t res = brwGetUpdate(baseSeq, resumeRow);
        if(res != BRW_OK) {
            resumeRow = 0;
            if(res == BRW_FAILED) {
                strcpy(localMsg, "Cosmo2ex is not answering");
            }
            return drawn;
        }

        uint32_t seq      = getDwordBE(dmaBuf);
        uint8_t  flags    = dmaBuf[4];
        uint8_t  frameRes = dmaBuf[5];
        uint16_t rects    = getWordBE(dmaBuf + 6);
        uint16_t palCount = getWordBE(dmaBuf + 8);

        if((flags & BRW_UPD_NO_CHANGE) || frameRes != curMode->res) {   // nothing new, or rendered for another mode
            resumeRow = 0;
            return drawn;
        }

        uint8_t *p = dmaBuf + BRW_UPD_HEADER_SIZE;
        mouseHide();
        if(flags & BRW_UPD_PALETTE) {
            setPalette((uint16_t *) p, palCount);           // big endian words - as the palette registers want them
        }
        p += palCount * 2;

        uint16_t nextRow = 0;
        while(rects--) {
            uint16_t x16 = getWordBE(p);
            uint16_t y   = getWordBE(p + 2);
            uint16_t w16 = getWordBE(p + 4);
            uint16_t h   = getWordBE(p + 6);
            p += BRW_UPD_RECT_HEADER_SIZE;

            uint16_t drawH = (y + h > pageH) ? ((y < pageH) ? pageH - y : 0) : h;
            if(drawH) {
                copyRect(x16, pageTop + y, w16, drawH, p);
            }
            p += (uint32_t) w16 * curMode->planes * 2 * h;
            nextRow = y + h;
        }
        mouseShow();
        drawn = TRUE;
        waitingForPage = FALSE;
        frameSinceScroll = TRUE;

        if(flags & BRW_UPD_MORE) {                          // next part right away
            resumeRow = nextRow;
            continue;
        }
        baseSeq   = seq;
        resumeRow = 0;
        return drawn;
    }
    return drawn;
}

//--------------------------------------------------
// URL line with suggestions

static void loadHistory(void)
{
    histCount = 0;
    if(brwGetHistory(BRW_HISTORY_MAX) != BRW_OK) {
        return;
    }

    uint16_t count = getWordBE(dmaBuf);
    uint32_t avail = (uint32_t) dmaBufSectors * 512 - 2;
    if(avail > sizeof(histData)) {
        avail = sizeof(histData);
    }
    memcpy(histData, dmaBuf + 2, avail);
    histData[sizeof(histData) - 1] = 0;

    char *s = histData;
    while(histCount < count && histCount < BRW_HISTORY_MAX && s < histData + avail && *s) {
        hist[histCount++] = s;
        s += strlen(s) + 1;
    }
}

#define SUGG_MAX    10

// returns TRUE when a URL was opened; FALSE when cancelled (or quit set)
static uint8_t urlEditor(const char *prefill, uint8_t canCancel)
{
    char text[256];
    char filter[256];                                       // what the suggestions are filtered by - what the user typed
    int  len, cur, sel = -1, start, i;
    char *match[SUGG_MAX];
    int  matchCount = 0;
    uint8_t redraw = TRUE, lastButtons = 0;
    uint16_t suggRow = 4;

    strncpy(text, prefill ? prefill : "", sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    len = cur = strlen(text);
    filter[0] = 0;                                          // the current URL put there doesn't filter

    loadHistory();
    needWholePage();
    mousePointer(0, 0);                                     // normal arrow - no scrolling here

    for(;;) {
        if(redraw) {
            redraw = FALSE;

            matchCount = 0;                                 // newest first: all of them, or those with the typed text
            for(i=0; i<histCount && matchCount<SUGG_MAX; i++) {
                if(containsNoCase(hist[i], filter)) {
                    match[matchCount++] = hist[i];
                }
            }
            int rowsLeft = textRows() - suggRow - 2;
            if(matchCount > rowsLeft) {
                matchCount = rowsLeft > 0 ? rowsLeft : 0;
            }
            if(sel >= matchCount) {
                sel = matchCount - 1;
            }

            mouseHide();
            clearPage();
            pageText(0, 1, "Open URL or search:", FALSE);

            // edit line as [text    ], scrolled so the cursor is visible
            int vis = textCols - 4;                         // inside the brackets
            start = (cur >= vis) ? cur - vis + 1 : 0;
            char shown[260];
            shown[0] = '[';
            for(i=0; i<vis; i++) {
                shown[1 + i] = (start + i < len) ? text[start + i] : ' ';
            }
            shown[1 + vis] = ']';
            shown[2 + vis] = 0;
            pageText(1, 1, shown, FALSE);
            if(textRows() > 1) {                            // cursor: just the one character inverted
                char c[2] = { (cur < len) ? text[cur] : ' ', 0 };
                drawText(2 + cur - start, pageTop + fontH, c, 1, TRUE);
            }
            fillRows(pageTop + 2 * fontH + fontH / 2, 1, 1);

            if(matchCount) {
                pageText(3, 1, filter[0] ? "Opened pages with this text:" : "Pages you opened recently:", FALSE);
            }
            for(i=0; i<matchCount; i++) {
                pageText(suggRow + i, 1, match[i], i == sel);
            }
            pageText(textRows() - 1, 1, "Enter open, Up/Down pick, Tab edit, Esc", FALSE);
            mouseShow();
        }

        Vsync();

        int16_t mx, my;
        uint8_t mb;
        mouseGet(&mx, &my, &mb);
        if((mb & 1) && !(lastButtons & 1) && my >= pageTop) {          // click on a suggestion opens it
            int row = (my - pageTop) / fontH - suggRow;
            if(row >= 0 && row < matchCount) {
                brwNavigate(match[row]);
                return TRUE;
            }
        }
        lastButtons = mb;

        Key k;
        if(!readKey(&k)) {
            continue;
        }
        redraw = TRUE;

        if(isQuitKey(&k)) {
            quit = TRUE;
            return FALSE;
        }

        switch(k.scan) {
            case SC_RETURN:
            case SC_ENTER: {
                const char *u = (sel >= 0) ? match[sel] : text;
                if(*u) {
                    brwNavigate(u);
                    return TRUE;
                }
                continue;
            }
            case SC_ESC:
                if(sel >= 0 || len) {                       // first Esc clears, the next one leaves
                    sel = -1;
                    len = cur = 0;
                    text[0] = filter[0] = 0;
                } else if(canCancel) {
                    return FALSE;
                }
                continue;
            case SC_UP:
                if(sel >= 0) {
                    sel--;
                }
                continue;
            case SC_DOWN:
                if(sel < matchCount - 1) {
                    sel++;
                }
                continue;
            case SC_TAB:
                if(sel >= 0) {                              // the picked URL to the line - the list stays as it is
                    strncpy(text, match[sel], sizeof(text) - 1);
                    len = cur = strlen(text);
                    sel = -1;
                }
                continue;
            case SC_LEFT:
                if(cur > 0) cur--;
                continue;
            case SC_RIGHT:
                if(cur < len) cur++;
                continue;
            case SC_HOME:
                if(k.shift & KB_SHIFT) {                    // Clr
                    len = cur = 0;
                    text[0] = filter[0] = 0;
                } else {
                    cur = 0;
                }
                continue;
            case SC_BACKSP:
                if(cur > 0) {
                    memcpy(text + cur - 1, text + cur, len - cur + 1);
                    cur--; len--;
                    sel = -1;
                    strcpy(filter, text);
                }
                continue;
            case SC_DELETE:
                if(cur < len) {
                    memcpy(text + cur, text + cur + 1, len - cur);
                    len--;
                    sel = -1;
                    strcpy(filter, text);
                }
                continue;
        }

        if(k.ascii >= 32 && k.ascii != 127 && len < (int) sizeof(text) - 1) {
            for(i=len; i>=cur; i--) {
                text[i + 1] = text[i];
            }
            text[cur++] = k.ascii;
            len++;
            sel = -1;
            strcpy(filter, text);
        }
    }
}

// URL line, then the page loads
static void openUrl(uint8_t canCancel)
{
    if(urlEditor(st.url[0] ? st.url : NULL, canCancel)) {
        ensureMode();                                       // after NAVIGATE - so ce_browser doesn't open its home page first
        readStatus();
        startWaiting();
    } else {
        mouseHide();
        clearPage();
        mouseShow();
    }
    needWholePage();
}

//--------------------------------------------------
// help and resolution menu

static void waitKeyOrClick(void)
{
    Key k;
    int16_t x, y;
    uint8_t b, last;

    mouseGet(&x, &y, &last);
    for(;;) {
        Vsync();
        if(readKey(&k)) {
            if(isQuitKey(&k)) {
                quit = TRUE;
            }
            return;
        }
        mouseGet(&x, &y, &b);
        if((b & 1) && !(last & 1)) {
            return;
        }
        last = b;
    }
}

static void helpScreen(void)
{
    static const char *lines[] = {
        "CE_BROWS - web browser for Cosmo2ex",
        "",
        "F1, Help      this help",
        "Ctrl+L, F3    open URL / search",
        "F2            screen resolution",
        "F4, Alt+Z     zoom 1x / 2x",
        "Arrows        move / scroll a bit",
        "Shift+Arrows  scroll by a page",
        "Mouse at edge scroll that way",
        "Click         links, buttons, fields",
        "Undo          back (Shift+Undo fwd)",
        "F5 reload     Esc stop loading",
        "Ctrl+Q, Ctrl+D, F10   quit",
        "Other keys go to the page.",
        "",
        NULL
    };
    uint16_t row;

    mousePointer(0, 0);
    mouseHide();
    clearPage();
    for(row=0; lines[row]; row++) {
        pageText(row, 1, lines[row], FALSE);
    }
    pageText(row++, 1, curMode->name, FALSE);
    pageText(row++, 1, (zoomQ == 4) ? "Zoom 1x" : "Zoom 2x", FALSE);
    pageText(row + 1, 1, "Press a key...", FALSE);
    mouseShow();

    waitKeyOrClick();
    mouseHide();
    clearPage();
    mouseShow();
    needWholePage();
}

static void applyVideoMode(const VMode *m)
{
    videoSetMode(m);
    layout();
    modeSent = FALSE;
    ensureMode();
    drawToolbar();
    needWholePage();
    startWaiting();
}

static void resolutionMenu(void)
{
    int n = videoModeCount();
    int sel = 0, i;

    for(i=0; i<n; i++) {
        if(videoModeAt(i) == curMode) {
            sel = i;
        }
    }
    mousePointer(0, 0);

    for(;;) {
        mouseHide();
        clearPage();
        pageText(0, 1, "Screen resolution:", FALSE);
        for(i=0; i<n; i++) {
            char line[60];
            line[0] = '1' + i;
            line[1] = ' ';
            line[2] = (videoModeAt(i) == curMode) ? '*' : ' ';
            line[3] = ' ';
            strncpy(line + 4, videoModeAt(i)->name, sizeof(line) - 5);
            line[sizeof(line) - 1] = 0;
            pageText(2 + i, 1, line, i == sel);
        }
        if(n == 1) {
            pageText(3 + n, 1, "This monitor shows only this one.", FALSE);
        }
        pageText(4 + n, 1, "Up/Down, Enter or 1-9; Esc back", FALSE);
        mouseShow();

        Key k;
        while(!readKey(&k)) {
            Vsync();
        }
        if(isQuitKey(&k)) {
            quit = TRUE;
            break;
        }
        if(k.scan == SC_ESC) {
            break;
        }
        if(k.scan == SC_UP && sel > 0) sel--;
        if(k.scan == SC_DOWN && sel < n - 1) sel++;
        if(k.ascii >= '1' && k.ascii < '1' + n) {
            sel = k.ascii - '1';
            k.scan = SC_RETURN;
        }
        if(k.scan == SC_RETURN || k.scan == SC_ENTER) {
            if(videoModeAt(sel) != curMode) {
                applyVideoMode(videoModeAt(sel));
                return;
            }
            break;
        }
    }

    mouseHide();
    clearPage();
    mouseShow();
    needWholePage();
}

//--------------------------------------------------
// the page: keys and mouse

static void handleKey(const Key *k)
{
    if(isQuitKey(k)) {
        quit = TRUE;
        return;
    }

    uint8_t shift = k->shift & KB_SHIFT;

    if(k->scan == SC_F1 || k->scan == SC_HELP) {
        helpScreen();
    } else if(k->scan == SC_F2) {
        resolutionMenu();
    } else if(k->scan == SC_F3 || k->ascii == CTRL_L) {
        openUrl(TRUE);
    } else if(k->scan == SC_F4 || (k->scan == SC_Z && (k->shift & KB_ALT))) {
        zoomQ = (zoomQ == 4) ? 8 : 4;
        modeSent = FALSE;
        ensureMode();
        drawToolbar();
        needWholePage();
        startWaiting();
    } else if(k->scan == SC_F5) {
        brwNavAction(BRW_NAV_RELOAD);
    } else if(k->scan == SC_ESC) {
        brwNavAction(BRW_NAV_STOP);
    } else if(k->scan == SC_UNDO) {
        brwNavAction(shift ? BRW_NAV_FORWARD : BRW_NAV_BACK);
    } else if(shift && k->scan == SC_UP) {
        brwScroll(0, -1, 0, 0);
    } else if(shift && k->scan == SC_DOWN) {
        brwScroll(0, 1, 0, 0);
    } else if(shift && k->scan == SC_LEFT) {
        brwScroll(0, 0, 0, -1);
    } else if(shift && k->scan == SC_RIGHT) {
        brwScroll(0, 0, 0, 1);
    } else {
        brwKey(k->scan, k->ascii, k->shift);                // to the page - typing in fields, arrows, Enter, ...
    }
}

#define EDGE_DWELL_TICKS    (TICKS_PER_S * 3 / 10)      // at the edge this long before scrolling starts
#define EDGE_WAIT_TICKS     (TICKS_PER_S * 4 / 10)      // next jump even without the frame of the previous one after this
#define EDGE_SPEED          800                         // CSS pixels per second, about

// Edge scrolling goes as fast as the frames can come: the next jump when the frame of the previous one is here, but
// not sooner than its transfer takes (1200 kB/s ACSI + a bit): ST frames 100 ms, TT 150 kB frames ~175 ms.
// The jump size keeps the speed the same either way.
static uint32_t edgeIntervalTicks(void)
{
    uint32_t frameBytes = (uint32_t) lineBytes * pageH;
    uint32_t ms = frameBytes / 1200 + 50;
    if(ms < 100) {
        ms = 100;
    }
    return ms * TICKS_PER_S / 1000;
}

static int8_t edgeJump(void)                            // lines / columns of 40 CSS pixels per jump
{
    uint32_t ms = edgeIntervalTicks() * 1000 / TICKS_PER_S;
    int8_t j = (int8_t) ((ms * EDGE_SPEED / 1000 + 20) / 40);
    return j < 1 ? 1 : j;
}

static int16_t  lastX = -1, lastY = -1;
static uint8_t  lastButtons;
static uint32_t edgeSince, edgeLast;

// TRUE if the user did something
static uint8_t handleMouse(void)
{
    int16_t x, y;
    uint8_t b;
    uint8_t active = FALSE;

    mouseGet(&x, &y, &b);
    uint8_t inPage = (y >= pageTop);
    uint16_t py = inPage ? y - pageTop : 0;

    if((x != lastX || y != lastY) && inPage) {
        brwMouse(BRW_MOUSE_MOVE, x, py, b);
        active = TRUE;
    }

    uint8_t changed = b ^ lastButtons;
    if(changed) {
        active = TRUE;
        if(inPage) {
            brwMouse((b & changed) ? BRW_MOUSE_DOWN : BRW_MOUSE_UP, x, py, b);
        } else if((b & 1) && !(lastButtons & 1)) {          // click on the toolbar
            int col = x / 8;
            if(col == 0) {
                brwNavAction(BRW_NAV_BACK);
            } else if(col == 1) {
                brwNavAction(BRW_NAV_FORWARD);
            } else {
                openUrl(TRUE);
            }
        }
    }
    lastX = x;
    lastY = y;
    lastButtons = b;

    // pointer in the 10 % border of the page: scroll that way, by jumps
    int8_t dx = 0, dy = 0;
    if(inPage && !b) {
        uint16_t w = curMode->width;
        if(x < w / 10)             dx = -1;
        if(x >= w - w / 10)        dx = 1;
        if(py < pageH / 10)        dy = -1;
        if(py >= pageH - pageH / 10) dy = 1;
    }
    mousePointer(dx, dy);                                   // scroll arrow in the border, else the normal arrow
    if(dx || dy) {
        uint32_t now = ticks();
        if(!edgeSince) {
            edgeSince = now;
        } else if(now - edgeSince >= EDGE_DWELL_TICKS && now - edgeLast >= edgeIntervalTicks() &&
                  (frameSinceScroll || now - edgeLast >= EDGE_WAIT_TICKS)) {
            int8_t j = edgeJump();
            brwScroll(dy * j, 0, dx * j, 0);
            edgeLast = now;
            frameSinceScroll = FALSE;
            active = TRUE;
        }
    } else {
        edgeSince = 0;
    }
    return active;
}

//--------------------------------------------------

int main(void)
{
    const char *err = NULL;

    (void) Cconws("\33E\r\nCE_BROWS - web browser for Cosmo2ex\r\n\r\n");

    if(!videoDetect(&err)) {
        (void) Cconws(err);
        (void) Cconws("Press a key.\r\n");
        Cnecin();
        return 0;
    }
    if(!brwInit()) {
        (void) Cconws("Not enough memory.\r\nPress a key.\r\n");
        Cnecin();
        return 0;
    }

    (void) Cconws("Looking for Cosmo2ex...\r\n");
    if(!brwFind()) {
        (void) Cconws("Cosmo2ex with the browser module not found.\r\nIs ce_cores up to date?\r\nPress a key.\r\n");
        Cnecin();
        return 0;
    }

    if(!videoStart()) {
        (void) Cconws("Not enough memory for the screen.\r\nPress a key.\r\n");
        Cnecin();
        return 0;
    }
    layout();

    readStatus();
    drawToolbar();
    openUrl(FALSE);                                         // nothing open yet - ask for the URL first

    uint32_t lastStatus = 0;
    uint16_t idleVbls = 0;
    uint16_t vbl = 0;

    while(!quit) {
        uint8_t active = FALSE;
        Key k;

        while(!quit && readKey(&k)) {
            handleKey(&k);
            active = TRUE;
        }
        if(quit) {
            break;
        }
        if(handleMouse()) {
            active = TRUE;
        }

        // after input or new frames poll every VBL, when quiet for 2 s only every 4th
        if(active || idleVbls < 100 || (vbl & 3) == 0) {
            if(pageUpdate()) {
                active = TRUE;
            }
        }
        idleVbls = active ? 0 : idleVbls + 1;

        uint32_t now = ticks();
        if(now - lastStatus >= TICKS_PER_S / 2) {
            lastStatus = now;
            updateStatus();
        }

        Vsync();
        vbl++;
    }

    videoEnd();
    return 0;
}
