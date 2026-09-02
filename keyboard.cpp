
#include <M5Unified.h>
#include "p4_os.h"
#include <cstring>
#include <cstdio>

static const int FIELD_H  = 74;
static const int ROW_H    = 76;
static const int ROW_GAP  = 10;
static const int PAD      = 16;
static const int KEY_GAP  = 8;
static const int NUM_CHAR_ROWS = 3;
static const int TOTAL_ROWS    = NUM_CHAR_ROWS + 1;

static int KB_HEIGHT = PAD * 2 + FIELD_H + PAD + TOTAL_ROWS * (ROW_H + ROW_GAP) - ROW_GAP;

static bool  isOpen    = false;
static bool  obscure   = false;
static bool  kbShift   = false;
static bool  kbSymbols = false;
static char* activeBuffer   = nullptr;
static int   activeCapacity = 0;
static int   activeLen      = 0;
static char  doneLabelStr[16]  = "Done";
static char  fieldLabelStr[64] = "";

// action: 0=char 1=shift 2=backspace 3=space 4=cancel 5=done 6=symbols toggle 7=spacer (no key)
struct KbKey { int x, y, w, h; char ch; int action; const char* label; uint32_t bg; };

static KbKey kbKeys[48];
static int   kbKeyCount = 0;

struct Cell { float weight; int action; char ch; const char* label; uint32_t bg; };

static const uint32_t COL_KEY = 0x3A3A50U;
static const uint32_t COL_FN  = 0x2C2C42U;

void keyboard_open(char* buffer, int capacity, bool obscure_,
                    const char* doneLabel, const char* fieldLabel) {
    activeBuffer   = buffer;
    activeCapacity = capacity;
    activeLen      = (int)strnlen(buffer, capacity);
    obscure        = obscure_;
    kbShift        = false;
    kbSymbols      = false;
    isOpen         = true;
    snprintf(doneLabelStr, sizeof(doneLabelStr), "%s", doneLabel ? doneLabel : "Done");
    snprintf(fieldLabelStr, sizeof(fieldLabelStr), "%s", fieldLabel ? fieldLabel : "");
}

void keyboard_close() {
    isOpen = false;
    activeBuffer = nullptr;
}

bool keyboard_is_open() {
    return isOpen;
}

int keyboard_height() {
    return isOpen ? KB_HEIGHT : 0;
}

static void draw_field(int barY) {
    auto& d = M5.Display;
    int W = d.width();

    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_STATUSBAR);
    if (fieldLabelStr[0]) {
        d.drawString(fieldLabelStr, PAD + 4, barY + 8);
    }

    int fx = PAD, fy = barY + 30, fw = W - PAD * 2, fh = FIELD_H - 34;
    d.fillRoundRect(fx, fy, fw, fh, 8, COL_CARD);

    char shown[80];
    if (obscure) {
        int n = activeLen < 78 ? activeLen : 78;
        for (int i = 0; i < n; i++) shown[i] = '*';
        shown[n] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", activeBuffer ? activeBuffer : "");
    }
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_CARD);
    d.drawString(shown, fx + 16, fy + fh / 2);
}

static void redraw_field_only() {
    auto& d = M5.Display;
    int barY = d.height() - KB_HEIGHT;
    draw_field(barY);
}

static void layout_row(const Cell* cells, int n, int y, int areaX, int areaW) {
    auto& d = M5.Display;
    float totalWeight = 0;
    for (int i = 0; i < n; i++) totalWeight += cells[i].weight;
    float unitW = (areaW - (n - 1) * KEY_GAP) / totalWeight;

    float kx = areaX;
    for (int i = 0; i < n; i++) {
        int x = (int)(kx + 0.5f);
        int w = (int)(unitW * cells[i].weight + 0.5f);
        const Cell& c = cells[i];

        if (c.action != 7) {
            kbKeys[kbKeyCount++] = { x, y, w, ROW_H, c.ch, c.action, c.label, c.bg };
        }

        d.fillRoundRect(x, y, w, ROW_H, 8, c.bg);
        d.setTextDatum(middle_center);
        if (c.label) {
            d.setFont(&fonts::Font2);
            d.setTextColor(COL_WHITE, c.bg);
            d.drawString(c.label, x + w / 2, y + ROW_H / 2);
        } else if (c.action == 0) {
            d.setFont(&fonts::Font4);
            d.setTextColor(COL_WHITE, c.bg);
            char s[2] = { c.ch, 0 };
            d.drawString(s, x + w / 2, y + ROW_H / 2);
        }

        kx += w + KEY_GAP;
    }
}

void keyboard_draw() {
    if (!isOpen) return;
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();
    int barY = H - KB_HEIGHT;
    kbKeyCount = 0;

    d.fillRect(0, barY, W, KB_HEIGHT, COL_STATUSBAR);
    d.drawFastHLine(0, barY, W, 0x333355U);

    draw_field(barY);

    int areaX = PAD, areaW = W - PAD * 2;
    int rowY  = barY + FIELD_H + PAD;

    if (!kbSymbols) {

        Cell row1[10];
        const char* r1 = "qwertyuiop";
        for (int i = 0; i < 10; i++) {
            char ch = r1[i];
            if (kbShift) ch = ch - 'a' + 'A';
            row1[i] = { 1.0f, 0, ch, nullptr, COL_KEY };
        }
        layout_row(row1, 10, rowY, areaX, areaW);

        Cell row2[11];
        const char* r2 = "asdfghjkl";
        row2[0] = { 0.5f, 7, 0, nullptr, COL_STATUSBAR };
        for (int i = 0; i < 9; i++) {
            char ch = r2[i];
            if (kbShift) ch = ch - 'a' + 'A';
            row2[i + 1] = { 1.0f, 0, ch, nullptr, COL_KEY };
        }
        row2[10] = { 0.5f, 7, 0, nullptr, COL_STATUSBAR };
        layout_row(row2, 11, rowY + (ROW_H + ROW_GAP), areaX, areaW);

        Cell row3[9];
        row3[0] = { 1.5f, 1, 0, kbShift ? "SHIFT*" : "shift", COL_FN };
        const char* r3 = "zxcvbnm";
        for (int i = 0; i < 7; i++) {
            char ch = r3[i];
            if (kbShift) ch = ch - 'a' + 'A';
            row3[i + 1] = { 1.0f, 0, ch, nullptr, COL_KEY };
        }
        row3[8] = { 1.5f, 2, 0, "Back", COL_FN };
        layout_row(row3, 9, rowY + 2 * (ROW_H + ROW_GAP), areaX, areaW);
    } else {

        Cell row1[10];
        const char* r1 = "1234567890";
        for (int i = 0; i < 10; i++) row1[i] = { 1.0f, 0, r1[i], nullptr, COL_KEY };
        layout_row(row1, 10, rowY, areaX, areaW);

        Cell row2[10];
        const char* r2 = "!@#$%^&*()";
        for (int i = 0; i < 10; i++) row2[i] = { 1.0f, 0, r2[i], nullptr, COL_KEY };
        layout_row(row2, 10, rowY + (ROW_H + ROW_GAP), areaX, areaW);

        Cell row3[9];
        row3[0] = { 1.5f, 7, 0, nullptr, COL_STATUSBAR };
        const char* r3 = ".,?!;:'";
        for (int i = 0; i < 7; i++) row3[i + 1] = { 1.0f, 0, r3[i], nullptr, COL_KEY };
        row3[8] = { 1.5f, 2, 0, "Back", COL_FN };
        layout_row(row3, 9, rowY + 2 * (ROW_H + ROW_GAP), areaX, areaW);
    }

    int fnY = rowY + NUM_CHAR_ROWS * (ROW_H + ROW_GAP);
    Cell fnKeys[4] = {
        { 1.5f, 6, 0, kbSymbols ? "ABC" : "123", COL_FN },
        { 3.0f, 3, 0, "Space",     COL_KEY },
        { 1.5f, 4, 0, "Cancel",    COL_DANGER },
        { 1.5f, 5, 0, doneLabelStr, COL_OK },
    };
    layout_row(fnKeys, 4, fnY, areaX, areaW);
}

static bool in_rect(int tx, int ty, int x, int y, int w, int h) {
    return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

KeyboardResult keyboard_touch(int tx, int ty) {
    KeyboardResult r;
    if (!isOpen) return r;
    int kb_last_action = -1;

    for (int i = 0; i < kbKeyCount; i++) {
        KbKey& k = kbKeys[i];
        if (!in_rect(tx, ty, k.x, k.y, k.w, k.h)) continue;

        r.handled = true;
        kb_last_action = k.action;
        switch (k.action) {
            case 0:
                if (activeBuffer && activeLen < activeCapacity - 1) {
                    activeBuffer[activeLen++] = k.ch;
                    activeBuffer[activeLen] = '\0';
                    r.changed = true;
                }
                break;
            case 1:
                kbShift = !kbShift;
                break;
            case 6:
                kbSymbols = !kbSymbols;
                break;
            case 3:
                if (activeBuffer && activeLen < activeCapacity - 1) {
                    activeBuffer[activeLen++] = ' ';
                    activeBuffer[activeLen] = '\0';
                    r.changed = true;
                }
                break;
            case 2:
                if (activeBuffer && activeLen > 0) {
                    activeBuffer[--activeLen] = '\0';
                    r.changed = true;
                }
                break;
            case 4:
                r.cancelled = true;
                break;
            case 5:
                r.submitted = true;
                break;
        }
        break;
    }

    if (r.handled && !r.submitted && !r.cancelled) {
        if (kb_last_action == 1 || kb_last_action == 6) {
            keyboard_draw();
        } else {
            redraw_field_only();
        }
    }
    return r;
}

