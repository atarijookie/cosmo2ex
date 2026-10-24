#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "panel.h"
#include "display.h"
#include "connection.h"
#include "host_receive.h"
#include "image_browser.h"

extern int imageState;

// the list - the page of it which is here
static bool     haveList;           // got a page since connecting
static bool     listRequested;      // asked for it since connecting - the host then sends the changes on its own
static uint32_t listGen, listTotal, pageStart;
static uint32_t listCurrent = IMAGE_LIST_NONE;
static uint16_t pageCount;
static char     pageNames[IMAGE_LIST_PAGE_MAX][IMAGE_LIST_NAME_MAX + 1];

// browsing
static bool     browsing;
static uint32_t cursor;                                 // index of the shown image
static char     cursorName[IMAGE_LIST_NAME_MAX + 1];    // its name, "" while its page isn't here
static uint32_t lastKeyTime;
static bool     requestPending;
static uint32_t requestTime;
static bool     insertPending;
static const char* statusText;                          // on the last line instead of the hint, NULL = none
static uint32_t statusTime;
static bool     displayDirty;

static const char* nameAt(uint32_t index)
{
    if(!haveList || index < pageStart || index >= pageStart + pageCount) {
        return NULL;
    }

    return pageNames[index - pageStart];
}

static void updateCursorName(void)
{
    const char* name = nameAt(cursor);
    strcpy(cursorName, name ? name : "");
}

static void requestPage(uint32_t start)
{
    uint8_t bfr[TX_HEADER_SIZE + 6];
    storeHeader(bfr, ATN_GET_IMAGE_LIST, 6);
    storeDword(bfr + TX_HEADER_SIZE, start);
    storeWord(bfr + TX_HEADER_SIZE + 4, IMAGE_LIST_PAGE_MAX);

    if(sendHeaderAndDataToHost(bfr, 6)) {
        listRequested = true;
        requestPending = true;
        requestTime = millis();
    }
}

// cursor not in the page, or close to its edge (and there's more of the list beyond it)?
static bool cursorNeedsPage(void)
{
    if(!haveList || listTotal == 0) {
        return false;
    }

    uint32_t pageEnd = pageStart + pageCount;       // first index after the page

    if(cursor < pageStart || cursor >= pageEnd) {
        return true;
    }

    if((cursor - pageStart) < BROWSE_PAGE_MARGIN && pageStart > 0) {
        return true;
    }

    return (pageEnd - 1 - cursor) < BROWSE_PAGE_MARGIN && pageEnd < listTotal;
}

static void sendInsert(void)
{
    uint8_t bfr[TX_HEADER_SIZE + 9 + IMAGE_LIST_NAME_MAX];
    uint8_t len = strlen(cursorName);

    storeHeader(bfr, ATN_INSERT_IMAGE, 9 + len);
    storeDword(bfr + TX_HEADER_SIZE, listGen);
    storeDword(bfr + TX_HEADER_SIZE + 4, cursor);
    bfr[TX_HEADER_SIZE + 8] = len;
    memcpy(bfr + TX_HEADER_SIZE + 9, cursorName, len);

    if(sendHeaderAndDataToHost(bfr, 9 + len)) {
        insertPending = true;
        statusText = "loading...";
        statusTime = millis();
        displayDirty = true;
    }
}

static void stopBrowsing(void)
{
    browsing = false;
    insertPending = false;
    statusText = NULL;

    if(imageState == IMAGE_LOADED) {    // otherwise the image loading has the display
        showRunningStateOnDisplay();
    }
}

static void showBrowser(void)
{
    displayDirty = false;

    const char* name = cursorName[0] ? cursorName : "...";      // page with it not here yet
    bool inDrive = (cursor == listCurrent);
    char line[PANEL_TEXT_MAX + 1];

    if(panelLines() >= 3) {
        panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, name);
        panelSetLine(1, PANEL_LEFT, 0, "");

        snprintf(line, sizeof(line), "< %lu/%lu >", (unsigned long) cursor + 1, (unsigned long) listTotal);
        panelSetLine(2, PANEL_CENTER, 0, line);

        panelSetLine(3, PANEL_CENTER, 0, statusText ? statusText : (inDrive ? "in drive" : "enter: insert"));
    } else {                            // small panel - index before the name, scrolls when it doesn't fit
        snprintf(line, sizeof(line), "%lu %.50s", (unsigned long) cursor + 1, name);   // 10 digits max + space + 50 fits the line
        panelSetLine(0, PANEL_LEFT, 0, line);
        panelSetLine(1, PANEL_LEFT, 0, statusText ? statusText : "");
    }
}

void browserReset(void)
{
    haveList = false;
    listRequested = false;
    requestPending = false;
    browsing = false;
    insertPending = false;
    statusText = NULL;
    listGen = listTotal = pageStart = 0;
    listCurrent = IMAGE_LIST_NONE;
    pageCount = 0;
    cursor = 0;
    cursorName[0] = 0;
}

bool browserActive(void)
{
    return browsing;
}

void browserLoop(void)
{
    if(!hostConnected()) {
        return;
    }

    uint32_t now = millis();

    if(requestPending && (now - requestTime) >= BROWSE_REQUEST_RETRY_MS) {    // no reply? ask again below
        requestPending = false;
    }

    if(!requestPending) {
        if(!haveList && panelPresent() && imageState == IMAGE_LOADED) {     // first time, or no reply yet
            requestPage(IMAGE_LIST_NONE);
        } else if(browsing && cursorNeedsPage()) {
            requestPage(cursor >= IMAGE_LIST_PAGE_MAX / 2 ? cursor - IMAGE_LIST_PAGE_MAX / 2 : 0);
        }
    }

    if(!browsing) {
        return;
    }

    if(imageState != IMAGE_LOADED) {    // other image is loading (e.g. from the web UI) - that has the display now
        browsing = false;
        insertPending = false;
        statusText = NULL;
        return;
    }

    if(insertPending && (now - statusTime) >= BROWSE_INSERT_TIMEOUT_MS) {   // no reply - let the user try again
        insertPending = false;
        statusText = "no reply";
        statusTime = now;
        displayDirty = true;
    }

    if((now - lastKeyTime) >= BROWSE_TIMEOUT_MS && !insertPending) {
        stopBrowsing();
        return;
    }

    if(statusText && !insertPending && (now - statusTime) >= BROWSE_STATUS_MS) {
        statusText = NULL;
        displayDirty = true;
    }

    if(displayDirty) {
        showBrowser();
    }
}

bool browserKey(char button, char event)
{
    if(event != 'S') {                  // long presses are not used here
        return false;
    }

    if(button == 'L' || button == 'R') {
        if(!haveList || listTotal == 0 || insertPending) {
            return false;
        }

        if(!browsing) {                 // start from the current image - or, when it's not in the list, from the first / last
            browsing = true;
            cursor = (listCurrent < listTotal) ? listCurrent : ((button == 'R') ? (listTotal - 1) : 0);
        }

        if(button == 'R') {             // wraps around at the ends, the same as the web UI
            cursor = (cursor + 1 >= listTotal) ? 0 : (cursor + 1);
        } else {
            cursor = (cursor == 0) ? (listTotal - 1) : (cursor - 1);
        }

        lastKeyTime = millis();
        statusText = NULL;
        updateCursorName();
        displayDirty = true;
        return true;
    }

    if(button == 'E' && browsing) {
        lastKeyTime = millis();

        if(cursor == listCurrent) {     // this one is in the drive already
            stopBrowsing();
        } else if(cursorName[0] && !insertPending) {    // no name yet - its page is on the way, enter again then
            sendInsert();
        }
        return true;
    }

    return false;
}

void browserImageListReceived(const uint8_t* data, uint32_t len)
{
    if(len < IMAGE_LIST_PAGE_HEADER) {
        debug("browserImageListReceived - too short: %d\n", (int) len);
        return;
    }

    requestPending = false;

    uint8_t* d = (uint8_t*) data;
    listGen     = getDword(d);
    listTotal   = getDword(d + 4);
    listCurrent = getDword(d + 8);
    pageStart   = getDword(d + 12);
    uint16_t count = MIN(getWord(d + 16), IMAGE_LIST_PAGE_MAX);

    uint32_t pos = IMAGE_LIST_PAGE_HEADER;
    pageCount = 0;

    while(pageCount < count && pos < len) {     // stops at a broken name, the page is shorter then
        uint8_t nameLen = data[pos++];
        if(pos + nameLen > len) {
            break;
        }

        memcpy(pageNames[pageCount], data + pos, nameLen);
        pageNames[pageCount][nameLen] = 0;
        pos += nameLen;
        pageCount++;
    }

    if(pageStart > listTotal || pageCount > listTotal - pageStart) {    // page beyond the list? none
        pageCount = 0;
    }

    haveList = true;
    debug("image list: gen %u, total %u, current %d, page %u + %u\n", (unsigned) listGen, (unsigned) listTotal, (int) listCurrent, (unsigned) pageStart, (unsigned) pageCount);

    if(!browsing) {
        return;
    }

    if(listTotal == 0) {                // nothing to browse anymore
        stopBrowsing();
        return;
    }

    // keep showing the same image if it's still there (other images added / deleted before it), otherwise the same index
    bool found = false;
    for(uint32_t i = 0; cursorName[0] && i < pageCount; i++) {
        if(strcmp(pageNames[i], cursorName) == 0) {
            cursor = pageStart + i;
            found = true;
            break;
        }
    }

    if(!found && cursor >= listTotal) {
        cursor = listTotal - 1;
    }

    updateCursorName();
    displayDirty = true;
}

void browserInsertStatusReceived(uint8_t status)
{
    if(!insertPending) {
        return;
    }

    insertPending = false;

    if(status == INSERT_IMAGE_OK) {     // the image follows - show it's coming until the loading progress does
        browsing = false;
        statusText = NULL;
        displayMessage("Loading image", cursorName, "");
        return;
    }

    statusText = (status == INSERT_IMAGE_NOT_FOUND) ? "not found" : "can't insert";
    statusTime = millis();
    displayDirty = true;
}
