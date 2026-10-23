#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <cstring>

#include "defs.h"
#include "protocol.h"
#include "screen.h"

// 0.91" 128x32 SSD1306 OLED - for another display change this, the fonts and LINE_HEIGHT
static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

#define LINES           4
#define LINE_HEIGHT     8
#define FONT_NORMAL     u8g2_font_5x8_tr        // bold = drawn twice, 1 px apart
#define FONT_DOUBLE     u8g2_font_7x14_tr
#define FONT_DOUBLE_B   u8g2_font_7x14B_tr
#define CHAR_W_NORMAL   5
#define CHAR_W_DOUBLE   7

#define SCROLL_START    0       // waiting at the start of the text
#define SCROLL_MOVING   1
#define SCROLL_END      2       // waiting at the end of the text

typedef struct {
    char     text[MAX_TEXT_LEN + 1];
    uint8_t  justify;
    uint8_t  flags;
    int16_t  scrollPos;
    uint8_t  scrollState;
    uint32_t scrollTime;        // when the scroll state started / last step
} Line;

static Line lines[LINES];
static bool dirty;
static uint32_t lastChange;
static uint32_t lastActivity;
static uint8_t brightness = DEFAULT_CONTRAST;
static bool dimmed;

void screenInit(void)
{
    Wire.setSDA(PIN_SDA);
    Wire.setSCL(PIN_SCL);
    u8g2.setBusClock(400000);
    u8g2.begin();
    u8g2.setFontPosTop();
    u8g2.setFontMode(1);        // transparent, for the bold and inverted text
    u8g2.setContrast(brightness);

    memset(lines, 0, sizeof(lines));
    dirty = true;
    lastChange = lastActivity = millis();
}

uint8_t screenLines(void)
{
    return LINES;
}

uint8_t screenCols(void)
{
    return u8g2.getDisplayWidth() / CHAR_W_NORMAL;
}

uint8_t screenCols2(void)
{
    return u8g2.getDisplayWidth() / CHAR_W_DOUBLE;
}

static void changed(uint32_t now)
{
    dirty = true;
    lastChange = now;
    screenActivity(now);
}

void screenClear(uint32_t now)
{
    memset(lines, 0, sizeof(lines));
    changed(now);
}

void screenSetLine(uint8_t line, uint8_t justify, uint8_t flags, const char* text, uint32_t now)
{
    if(line >= LINES) {
        return;
    }

    Line& l = lines[line];

    // the same content again (e.g. the FDD refreshing the whole screen) - not a change, keeps the scroll and dimming
    if(l.justify == justify && l.flags == flags && strcmp(l.text, text) == 0) {
        return;
    }

    strncpy(l.text, text, MAX_TEXT_LEN);
    l.text[MAX_TEXT_LEN] = 0;
    l.justify = justify;
    l.flags = flags;
    l.scrollPos = 0;
    l.scrollState = SCROLL_START;
    l.scrollTime = now;

    changed(now);
}

void screenSetBrightness(uint8_t value, uint32_t now)
{
    brightness = value;
    dimmed = false;
    u8g2.setContrast(brightness);
    lastActivity = now;
}

void screenActivity(uint32_t now)
{
    lastActivity = now;

    if(dimmed) {
        dimmed = false;
        u8g2.setContrast(brightness);
    }
}

static bool isDouble(int i)
{
    return (lines[i].flags & TEXT_DOUBLE) && i + 1 < LINES;     // double size on the last line is drawn normal
}

static void selectFont(int i)
{
    if(isDouble(i)) {
        u8g2.setFont((lines[i].flags & TEXT_BOLD) ? FONT_DOUBLE_B : FONT_DOUBLE);
    } else {
        u8g2.setFont(FONT_NORMAL);
    }
}

static int textWidth(int i, const char* text)
{
    selectFont(i);
    int w = u8g2.getStrWidth(text);

    if((lines[i].flags & TEXT_BOLD) && !isDouble(i)) {
        w++;
    }
    return w;
}

// printable ASCII only, the rest (incl. the reserved icon codes) as space
static void sanitize(const char* in, char* out)
{
    for(; *in; in++) {
        *out++ = (*in >= 0x20 && *in <= 0x7e) ? *in : ' ';
    }
    *out = 0;
}

static void draw(void)
{
    int width = u8g2.getDisplayWidth();
    char text[MAX_TEXT_LEN + 1];

    u8g2.clearBuffer();

    for(int i = 0; i < LINES; i++) {
        if(i > 0 && isDouble(i - 1)) {      // covered by the double size line above
            continue;
        }

        Line& l = lines[i];
        int y = i * LINE_HEIGHT;
        int h = isDouble(i) ? 2 * LINE_HEIGHT : LINE_HEIGHT;

        sanitize(l.text, text);
        int w = textWidth(i, text);     // also sets the font

        int x;
        if(w > width) {                 // doesn't fit - scrolled or cut off on the right
            x = (l.flags & TEXT_SCROLL) ? -l.scrollPos : 0;
        } else if(l.justify == JUSTIFY_CENTER) {
            x = (width - w) / 2;
        } else if(l.justify == JUSTIFY_RIGHT) {
            x = width - w;
        } else {
            x = 0;
        }

        u8g2.setClipWindow(0, y, width, y + h);

        u8g2.setDrawColor(1);
        if(l.flags & TEXT_INVERTED) {
            u8g2.drawBox(0, y, width, h);
            u8g2.setDrawColor(0);
        }

        u8g2.drawStr(x, y, text);
        if((l.flags & TEXT_BOLD) && !isDouble(i)) {
            u8g2.drawStr(x + 1, y, text);
        }
    }

    u8g2.setMaxClipWindow();
    u8g2.setDrawColor(1);
    u8g2.sendBuffer();
}

// moves the scrolled lines, returns true if anything moved
static bool scroll(uint32_t now)
{
    int width = u8g2.getDisplayWidth();
    char text[MAX_TEXT_LEN + 1];
    bool moved = false;

    for(int i = 0; i < LINES; i++) {
        Line& l = lines[i];

        if(!(l.flags & TEXT_SCROLL) || (i > 0 && isDouble(i - 1))) {
            continue;
        }

        sanitize(l.text, text);
        int overflow = textWidth(i, text) - width;
        if(overflow <= 0) {
            continue;
        }

        uint32_t elapsed = now - l.scrollTime;

        switch(l.scrollState) {
            case SCROLL_START:
                if(elapsed >= SCROLL_PAUSE_MS) {
                    l.scrollState = SCROLL_MOVING;
                    l.scrollTime = now;
                }
                break;

            case SCROLL_MOVING:
                if(elapsed >= SCROLL_STEP_MS) {
                    l.scrollTime = now;
                    l.scrollPos++;
                    moved = true;

                    if(l.scrollPos >= overflow) {
                        l.scrollState = SCROLL_END;
                    }
                }
                break;

            case SCROLL_END:
                if(elapsed >= SCROLL_PAUSE_MS) {
                    l.scrollState = SCROLL_START;
                    l.scrollTime = now;
                    l.scrollPos = 0;
                    moved = true;
                }
                break;
        }
    }

    return moved;
}

void screenLoop(uint32_t now)
{
    if(scroll(now)) {
        dirty = true;
    }

    if(dirty && now - lastChange >= REDRAW_DELAY_MS) {
        dirty = false;
        draw();
    }

    if(!dimmed && now - lastActivity >= DIM_AFTER_MS) {
        dimmed = true;
        u8g2.setContrast(DIM_CONTRAST);
    }
}
