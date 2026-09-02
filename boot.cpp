
#include <M5Unified.h>
#include "p4_os.h"

static const uint32_t BOOT_DURATION_MS = 3500;
static const uint32_t DOT_INTERVAL_MS  = 400;

static uint32_t lastDotMs   = 0;
static uint8_t  dotPhase    = 0;
static bool     dotDrawn    = false;

void boot_draw() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(COL_BLACK);

    d.setTextDatum(middle_center);
    d.setTextColor(COL_WHITE, COL_BLACK);
    d.setFont(&fonts::Font8);

    String vendor = VENDOR_NAME;
    d.drawString(vendor, W / 2, H * 2 / 5);

    int lineY = H / 2 + 20;
    d.drawFastHLine(W / 2 - 80, lineY, 160, 0x333333U);

    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_BLACK);
    d.setTextDatum(bottom_center);
    d.drawString("Powered by " OS_NAME, W / 2, H - 40);

    dotPhase = 0;
    dotDrawn = false;
    lastDotMs = millis();
}

static void draw_dots() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    int cy   = H * 2 / 3;
    int gap  = 24;
    int r    = 6;

    for (int i = 0; i < 3; i++) {
        int cx = W / 2 + (i - 1) * gap;
        uint32_t col = (i == dotPhase) ? COL_WHITE : 0x444444U;
        d.fillCircle(cx, cy, r, col);
    }
}

void boot_update() {
    uint32_t now = millis();

    if (now - lastDotMs >= DOT_INTERVAL_MS) {
        lastDotMs = now;
        dotPhase  = (dotPhase + 1) % 3;
        draw_dots();
    }

    if (now - os.bootStartMs >= BOOT_DURATION_MS) {
        os_goto(Screen::HOME);
    }
}

