#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"

#include "defs.h"
#include "utils.h"
#include "panel.h"

#define TX_QUEUE_SIZE   512
#define RX_LINE_MAX     80          // same as MAX_MSG_LEN in ce_pico_display/protocol.h

typedef struct {
    char    justify;
    uint8_t flags;
    char    text[PANEL_TEXT_MAX + 1];
} PanelLine;

static PanelLine wanted[PANEL_LINES];   // what should be on the panel
static PanelLine sent[PANEL_LINES];     // what was queued for it
static bool sentValid[PANEL_LINES];     // false = send this line even if it looks the same

static char txQueue[TX_QUEUE_SIZE];     // ring buffer
static size_t txHead, txCount;

static char rxLine[RX_LINE_MAX];
static size_t rxLen;
static bool rxOverflow;

static bool present;
static int lines = PANEL_LINES;
static int cols, cols2;                 // 0 = not known yet

static size_t txFree(void)
{
    return TX_QUEUE_SIZE - txCount;
}

// queues the whole message or nothing - a half sent message would be garbage for the panel
static bool queue(const char* msg)
{
    size_t len = strlen(msg);
    if(len > txFree()) {
        return false;
    }

    for(size_t i = 0; i < len; i++) {
        txQueue[(txHead + txCount) % TX_QUEUE_SIZE] = msg[i];
        txCount++;
    }
    return true;
}

static void send(void)
{
    size_t writable = panelUartWritable();

    while(txCount > 0 && writable > 0) {
        size_t chunk = MIN(txCount, TX_QUEUE_SIZE - txHead);   // up to the end of the ring buffer
        chunk = MIN(chunk, writable);

        size_t written = panelUartWrite(txQueue + txHead, chunk);
        if(written == 0) {
            break;
        }

        txHead = (txHead + written) % TX_QUEUE_SIZE;
        txCount -= written;
        writable -= written;
    }
}

// the flags the line is sent with - scroll added when the text doesn't fit the panel
static uint8_t effectiveFlags(int i)
{
    const PanelLine& l = wanted[i];
    uint8_t flags = l.flags;

    int lineCols = (flags & PANEL_DOUBLE) ? cols2 : cols;
    if(lineCols > 0 && (int) strlen(l.text) > lineCols) {
        flags |= PANEL_SCROLL;
    }
    return flags;
}

static void queueChangedLines(void)
{
    for(int i = 0; i < lines && i < PANEL_LINES; i++) {
        PanelLine now = wanted[i];
        now.flags = effectiveFlags(i);

        if(sentValid[i] && sent[i].justify == now.justify && sent[i].flags == now.flags &&
           strcmp(sent[i].text, now.text) == 0) {
            continue;
        }

        char msg[8 + PANEL_TEXT_MAX + 2];
        char* p = msg;
        *p++ = 'T';
        *p++ = '0' + i;
        *p++ = now.justify;
        if(now.flags & PANEL_BOLD)     *p++ = 'B';
        if(now.flags & PANEL_INVERTED) *p++ = 'I';
        if(now.flags & PANEL_DOUBLE)   *p++ = '2';
        if(now.flags & PANEL_SCROLL)   *p++ = 'S';
        sprintf(p, ":%s\n", now.text);

        if(!queue(msg)) {       // no space now - stays changed, next time
            return;
        }

        sent[i] = now;
        sentValid[i] = true;
    }
}

// panel (re)started or answered '?' - send everything again
static void resync(void)
{
    // drop the old queued stuff, the whole screen goes again; the '\n' ends a message that was sent only partially
    txCount = 0;
    queue("\nC\n");

    for(int i = 0; i < PANEL_LINES; i++) {
        sentValid[i] = false;
    }
}

static void handleIdentify(const char* msg)
{
    int proto, l, c, c2;
    if(sscanf(msg, "I%d,%d,%d,%d", &proto, &l, &c, &c2) != 4 || l < 1 || c < 1) {
        debug("panel - bad identify: %s\n", msg);
        return;
    }

    present = true;
    lines = MIN(l, PANEL_LINES);
    cols = c;
    cols2 = c2;
    debug("panel - %s\n", msg);

    resync();
}

static void handleMessage(const char* msg)
{
    if(msg[0] == 'I') {
        handleIdentify(msg);
    } else if(msg[0] == 'K' && msg[1] && msg[2] && !msg[3]) {
        onPanelKey(msg[1], msg[2]);
    }
}

static void receive(void)
{
    int c;
    while((c = panelUartRead()) >= 0) {
        if(c == '\r') {
            continue;
        }

        if(c == '\n') {
            rxLine[rxLen] = 0;
            if(!rxOverflow) {
                handleMessage(rxLine);
            }
            rxLen = 0;
            rxOverflow = false;
            continue;
        }

        if(rxLen >= RX_LINE_MAX - 1) {      // too long - dropped up to the next '\n'
            rxOverflow = true;
            continue;
        }
        rxLine[rxLen++] = c;
    }
}

void panelInit(void)
{
    memset(wanted, 0, sizeof(wanted));
    for(int i = 0; i < PANEL_LINES; i++) {
        wanted[i].justify = PANEL_LEFT;
        sentValid[i] = false;
    }

    txHead = txCount = 0;
    rxLen = 0;
    rxOverflow = false;
    present = false;
    lines = PANEL_LINES;
    cols = cols2 = 0;

    panelUartInit();
    resync();
    queue("?\n");               // ask the panel for its size; if it starts later, it announces itself
}

void panelClear(void)
{
    for(int i = 0; i < PANEL_LINES; i++) {
        panelSetLine(i, PANEL_LEFT, 0, "");
    }
}

void panelSetLine(int line, char justify, uint8_t flags, const char* text)
{
    if(line < 0 || line >= PANEL_LINES) {
        return;
    }

    PanelLine& l = wanted[line];
    l.justify = justify;
    l.flags = flags;            // + scroll when needed, see effectiveFlags()

    // printable ASCII only - '\n' would end the message
    int i = 0;
    for(; text && text[i] && i < PANEL_TEXT_MAX; i++) {
        l.text[i] = (text[i] >= 0x20 && text[i] <= 0x7e) ? text[i] : ' ';
    }
    l.text[i] = 0;
}

void panelBeep(uint16_t ms, uint16_t hz)
{
    char msg[24];
    if(hz) {
        sprintf(msg, "B%u,%u\n", ms, hz);
    } else {
        sprintf(msg, "B%u\n", ms);
    }
    queue(msg);                 // queue full - the beep is dropped
}

void panelLoop(void)
{
    receive();
    queueChangedLines();
    send();
}

void panelFlush(uint32_t timeoutMs)
{
    uint32_t start = millis();

    panelLoop();
    while(txCount > 0 && (millis() - start) < timeoutMs) {
        sleep_ms(1);
        send();
    }
}

bool panelPresent(void)
{
    return present;
}

int panelLines(void)
{
    return lines;
}
