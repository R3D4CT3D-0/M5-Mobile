#include <M5Unified.h>
#include "p4_os.h"
#include <cstring>
#include <cstdio>

static const int STATUS_H   = 48;
static const int ICON_W     = 200;
static const int ICON_H     = 220;
static const int ICON_R     = 28;
static const int ICON_COLS  = 2;
static const int ICON_GAP   = 50;

struct AppIcon {
    const char* label;
    uint32_t    bgColor;
    uint32_t    fgColor;
    const char* emoji;
    Screen      target;
    int         x, y, w, h;
};

static AppIcon apps[] = {
    { "Clock",       0x1A73E8U, COL_WHITE, "O",  Screen::APP_CLOCK,    0,0,0,0 },
    { "System Info", 0x34A853U, COL_WHITE, "#",  Screen::APP_SYSINFO,  0,0,0,0 },
    { "Settings",    0x5F6368U, COL_WHITE, "S",  Screen::APP_SETTINGS, 0,0,0,0 },
    { "Notepad",     0xF4A030U, COL_WHITE, "N",  Screen::APP_NOTEPAD,  0,0,0,0 },
    { "Flappy Bird", 0x4EC0E9U, COL_WHITE, ">",  Screen::APP_FLAPPY,   0,0,0,0 },
    { "Camera",      0xD23669U, COL_WHITE, "@",  Screen::APP_CAMERA,   0,0,0,0 },
    { "Discord",     0x5865F2U, COL_WHITE, "D",  Screen::APP_DISCORD,  0,0,0,0 },
};
static const int APP_COUNT = 7;

static uint32_t lastTouchMs = 0;
static const uint32_t TOUCH_DEBOUNCE = 300;

static uint32_t lastBattRefreshMs = 0;
static const uint32_t BATT_REFRESH_INTERVAL = 3000;

static void draw_status_bar();
static void draw_battery_cluster();
static void draw_icon(int idx);
static void compute_icon_positions();
static int  hit_test(int tx, int ty);

// rough discharge curve for a 2S li-ion pack, used to map voltage to a percentage
struct VoltPoint { float volts; int pct; };
static const VoltPoint kVoltCurve[] = {
    { 8.40f, 100 },
    { 8.20f,  95 },
    { 8.00f,  88 },
    { 7.80f,  78 },
    { 7.60f,  65 },
    { 7.40f,  50 },
    { 7.20f,  35 },
    { 7.00f,  20 },
    { 6.80f,  10 },
    { 6.40f,   3 },
    { 6.00f,   0 },
};
static const int kVoltCurveLen = (int)(sizeof(kVoltCurve) / sizeof(kVoltCurve[0]));

static const float BATT_ALPHA   = 0.10f;
static float  battVoltFiltered  = -1.0f;

// exponential moving average so the readout doesn't jitter with load spikes
static void batt_update_sample() {
    float v = M5.Power.getBatteryVoltage() / 1000.0f;
    if (v <= 1.0f) return;

    if (battVoltFiltered < 0.0f) {
        battVoltFiltered = v;
    } else {
        battVoltFiltered = battVoltFiltered * (1.0f - BATT_ALPHA) + v * BATT_ALPHA;
    }
}

static int volt_to_percent(float v) {

    if (v >= kVoltCurve[0].volts) return 100;

    if (v <= kVoltCurve[kVoltCurveLen - 1].volts) return 0;

    for (int i = 0; i < kVoltCurveLen - 1; i++) {
        float vHi = kVoltCurve[i].volts;
        float vLo = kVoltCurve[i + 1].volts;
        if (v <= vHi && v >= vLo) {
            float frac = (v - vLo) / (vHi - vLo);
            int pHi = kVoltCurve[i].pct;
            int pLo = kVoltCurve[i + 1].pct;
            return pLo + (int)(frac * (float)(pHi - pLo) + 0.5f);
        }
    }
    return 0;
}

int battery_get_percent() {
    if (battVoltFiltered < 0.0f) return -1;
    return volt_to_percent(battVoltFiltered);
}

bool battery_is_charging() {
    return M5.Power.isCharging();
}

void home_draw() {

    rotation_set_auto(true);
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(COL_SURFACE);

    if (os.wallpaperGrid) {
        for (int y = STATUS_H; y < H; y += 80) {
            d.drawFastHLine(0, y, W, 0x252538U);
        }
        for (int x = 0; x < W; x += 80) {
            d.drawFastVLine(x, STATUS_H, H - STATUS_H, 0x252538U);
        }
    }

    batt_update_sample();
    draw_status_bar();
    compute_icon_positions();

    for (int i = 0; i < APP_COUNT; i++) {
        draw_icon(i);
    }

    d.setTextDatum(bottom_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.drawString("Tap an app to open it", W / 2, H - 12);

    lastBattRefreshMs = millis();
}

static void compute_icon_positions() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    int rows   = (APP_COUNT + ICON_COLS - 1) / ICON_COLS;
    int totalW = ICON_COLS * ICON_W + (ICON_COLS - 1) * ICON_GAP;
    int totalH = rows * ICON_H + (rows - 1) * ICON_GAP;
    int startX = (W - totalW) / 2;
    int startY = STATUS_H + (H - STATUS_H - navbar_height() - totalH) / 2;

    for (int i = 0; i < APP_COUNT; i++) {
        int col = i % ICON_COLS;
        int row = i / ICON_COLS;
        apps[i].x = startX + col * (ICON_W + ICON_GAP);
        apps[i].y = startY + row * (ICON_H + ICON_GAP);
        apps[i].w = ICON_W;
        apps[i].h = ICON_H;
    }
}

static void draw_icon(int idx) {
    auto& d = M5.Display;
    AppIcon& a = apps[idx];

    d.fillRoundRect(a.x + 4, a.y + 6, a.w, a.h, ICON_R, 0x0A0A1AU);

    d.fillRoundRect(a.x, a.y, a.w, a.h, ICON_R, a.bgColor);

    d.drawRoundRect(a.x, a.y, a.w, a.h, ICON_R, 0xFFFFFF22);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8);
    d.setTextColor(a.fgColor, a.bgColor);
    d.drawString(a.emoji, a.x + a.w / 2, a.y + a.h / 2 - 18);

    d.setFont(&fonts::Font4);
    d.setTextColor(a.fgColor, a.bgColor);
    d.drawString(a.label, a.x + a.w / 2, a.y + a.h - 30);
}

static void draw_status_bar() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillRect(0, 0, W, STATUS_H, COL_STATUSBAR);
    d.drawFastHLine(0, STATUS_H, W, 0x333355U);

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(os.accentColor, COL_STATUSBAR);
    d.drawString(OS_NAME " " OS_VERSION, 16, STATUS_H / 2);

    draw_battery_cluster();
}

static void draw_battery_cluster() {
    auto& d = M5.Display;
    int W = d.width();

    const int glyphW  = 30;
    const int glyphH  = 16;
    const int tipW    = 4;
    const int tipH    = 8;
    int gx = W - 110;
    int gy = STATUS_H / 2 - glyphH / 2;

    int clearX = gx - 90;
    d.fillRect(clearX, 0, W - clearX, STATUS_H, COL_STATUSBAR);

    int percent   = battery_get_percent();
    bool charging = battery_is_charging();

    d.setTextDatum(middle_right);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_STATUSBAR);
    d.drawString("BATT", gx - 8, STATUS_H / 2);

    d.drawRect(gx, gy, glyphW, glyphH, COL_TEXT_DIM);
    d.fillRect(gx + glyphW + 1, gy + (glyphH - tipH) / 2, tipW, tipH, COL_TEXT_DIM);

    uint32_t fillColor = COL_OK;
    if (percent >= 0) {
        if (charging)        fillColor = 0xFFC107U;
        else if (percent <= 15) fillColor = COL_DANGER;
        else if (percent <= 30) fillColor = 0xFFA000U;

        int innerW = glyphW - 4;
        int fillW  = (innerW * percent) / 100;
        if (fillW < 0) fillW = 0;
        if (fillW > innerW) fillW = innerW;

        d.fillRect(gx + 2, gy + 2, innerW, glyphH - 4, COL_CARD);
        if (fillW > 0) {
            d.fillRect(gx + 2, gy + 2, fillW, glyphH - 4, fillColor);
        }
    } else {

        d.fillRect(gx + 2, gy + 2, glyphW - 4, glyphH - 4, COL_CARD);
    }

    char buf[16];
    if (percent >= 0) {
        if (charging) snprintf(buf, sizeof(buf), "%d%%+", percent);
        else          snprintf(buf, sizeof(buf), "%d%%", percent);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    d.setTextDatum(middle_right);
    d.setFont(&fonts::Font2);
    d.setTextColor(percent >= 0 ? COL_WHITE : COL_TEXT_DIM, COL_STATUSBAR);
    d.drawString(buf, W - 16, STATUS_H / 2);
}

static int hit_test(int tx, int ty) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (tx >= apps[i].x && tx < apps[i].x + apps[i].w &&
            ty >= apps[i].y && ty < apps[i].y + apps[i].h) {
            return i;
        }
    }
    return -1;
}

void home_update() {
    uint32_t now = millis();

    if (now - lastBattRefreshMs >= BATT_REFRESH_INTERVAL) {
        lastBattRefreshMs = now;
        batt_update_sample();
        draw_battery_cluster();
    }

    if (now - lastTouchMs < TOUCH_DEBOUNCE) return;

    auto t = M5.Touch.getDetail();
    if (t.wasClicked()) {
        int idx = hit_test(t.x, t.y);
        if (idx >= 0) {
            lastTouchMs = now;
            os_goto(apps[idx].target);
        }
    }
}

