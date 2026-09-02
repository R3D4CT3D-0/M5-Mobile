
#include <M5Unified.h>
#include "p4_os.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <cstdio>

static const int TITLEBAR_H  = 56;
static const int ROW_H       = 56;
static const int COL_LABEL_X = 60;
static const int COL_VALUE_X = 500;
static const int TABLE_TOP   = TITLEBAR_H + 40;

static uint32_t lastRefreshMs = 0;
static const uint32_t REFRESH_INTERVAL = 2000;

static void draw_titlebar();
static void draw_info_table();
static void draw_row(int row, const char* label, const char* value, uint32_t valColor);

void sysinfo_app_draw() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillScreen(COL_SURFACE);
    draw_titlebar();
    navbar_draw("System Info");

    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.setTextDatum(middle_left);
    d.drawString("PROPERTY", COL_LABEL_X, TABLE_TOP - 20);
    d.drawString("VALUE", COL_VALUE_X, TABLE_TOP - 20);
    d.drawFastHLine(COL_LABEL_X, TABLE_TOP - 4, W - COL_LABEL_X * 2, 0x333355U);

    draw_info_table();
    lastRefreshMs = millis();
}

static void draw_titlebar() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString("System Info", W / 2, TITLEBAR_H / 2);
}

static void draw_row(int row, const char* label, const char* value, uint32_t valColor) {
    auto& d = M5.Display;
    int W = d.width();
    int y = TABLE_TOP + row * ROW_H;

    uint32_t rowBg = (row % 2 == 0) ? COL_CARD : COL_SURFACE;
    d.fillRect(COL_LABEL_X - 10, y, W - COL_LABEL_X * 2 + 20, ROW_H - 2, rowBg);

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_TEXT_DIM, rowBg);
    d.drawString(label, COL_LABEL_X, y + ROW_H / 2);

    d.setTextColor(valColor, rowBg);
    d.drawString(value, COL_VALUE_X, y + ROW_H / 2);
}

static void draw_info_table() {
    char buf[64];

    draw_row(0, "OS", OS_NAME " v" OS_VERSION, COL_ACCENT);

    draw_row(1, "SoC", "ESP32-P4 (RISC-V)", COL_WHITE);

    snprintf(buf, sizeof(buf), "%u MHz", (unsigned)(ESP.getCpuFreqMHz()));
    draw_row(2, "CPU Freq", buf, COL_WHITE);

    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    draw_row(3, "Flash", buf, COL_WHITE);

    size_t psramFree  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psramTotal > 0) {
        snprintf(buf, sizeof(buf), "%u / %u MB free",
                 (unsigned)(psramFree / (1024 * 1024)),
                 (unsigned)(psramTotal / (1024 * 1024)));
    } else {
        snprintf(buf, sizeof(buf), "N/A");
    }
    draw_row(4, "PSRAM", buf, 0x00CC44U);

    size_t heapFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heapTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "%u / %u KB free",
             (unsigned)(heapFree / 1024),
             (unsigned)(heapTotal / 1024));
    draw_row(5, "Heap", buf, 0x00CC44U);

    auto& d = M5.Display;
    snprintf(buf, sizeof(buf), "%d x %d px", (int)d.width(), (int)d.height());
    draw_row(6, "Display", buf, COL_WHITE);

    uint32_t s  = millis() / 1000;
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             (unsigned)(s / 3600), (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
    draw_row(7, "Uptime", buf, COL_TEXT_DIM);
}

void sysinfo_app_update() {
    uint32_t now = millis();

    if (now - lastRefreshMs >= REFRESH_INTERVAL) {
        lastRefreshMs = now;
        draw_info_table();
    }

    auto t = M5.Touch.getDetail();
    if (t.wasClicked()) {
        navbar_touch(t.x, t.y);
    }
}

