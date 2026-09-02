
#include <M5Unified.h>
#include "p4_os.h"
#include <cstdio>

static const int TITLEBAR_H = 56;

static uint32_t lastSecMs  = 0;
static char     lastTime[16] = "";

static void draw_titlebar();
static void draw_clock(const char* timeStr);

void clock_app_draw() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(COL_SURFACE);
    draw_titlebar();
    navbar_draw("Clock");

    d.setTextDatum(top_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.drawString("Current Time", W / 2, TITLEBAR_H + 40);

    lastSecMs = 0;
    lastTime[0] = '\0';
}

static void draw_titlebar() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString("Clock", W / 2, TITLEBAR_H / 2);
}

static void draw_clock(const char* timeStr) {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height() - navbar_height();

    int cy = TITLEBAR_H + (H - TITLEBAR_H) / 2 + 20;

    int blockH = 120;
    d.fillRect(0, cy - blockH / 2, W, blockH, COL_SURFACE);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString(timeStr, W / 2, cy);

    d.setFont(&fonts::Font4);
    d.setTextColor(COL_ACCENT, COL_SURFACE);
    d.drawString("HH  :  MM  :  SS", W / 2, cy + 70);
}

void clock_app_update() {
    uint32_t now = millis();

    if (now - lastSecMs >= 1000) {
        lastSecMs = now;

        char timeStr[16];

        m5::rtc_time_t t;
        if (M5.Rtc.getTime(&t)) {
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
                     t.hours, t.minutes, t.seconds);
        } else {
            // no RTC available, fall back to uptime
            uint32_t s  = now / 1000;
            uint32_t h  = s / 3600;
            uint32_t m  = (s % 3600) / 60;
            uint32_t sc = s % 60;
            snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u",
                     (unsigned)h, (unsigned)m, (unsigned)sc);
        }

        if (strcmp(timeStr, lastTime) != 0) {
            strncpy(lastTime, timeStr, sizeof(lastTime));
            draw_clock(timeStr);
        }
    }

    auto t = M5.Touch.getDetail();
    if (t.wasClicked()) {
        navbar_touch(t.x, t.y);
    }
}

