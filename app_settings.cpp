#include <M5Unified.h>
#include "p4_os.h"
#include <WiFi.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>

static const int TITLEBAR_H = 56;
static const int TAB_BAR_H  = 48;
static const int CONTENT_TOP = TITLEBAR_H + TAB_BAR_H;

// which settings sub-page is showing
enum class SettingsTab { PERSONALIZATION, WIFI, RTC };
static SettingsTab currentTab = SettingsTab::PERSONALIZATION;

struct TabBtn { const char* label; SettingsTab tab; int x, y, w, h; };
static TabBtn tabBtns[] = {
    { "Personalization", SettingsTab::PERSONALIZATION, 0,0,0,0 },
    { "Wi-Fi",            SettingsTab::WIFI,             0,0,0,0 },
    { "RTC",              SettingsTab::RTC,              0,0,0,0 },
};
static const int TAB_COUNT = 3;

static void draw_titlebar_and_tabs();
static int  touch_tab(int tx, int ty);

static void draw_personalization();
static void touch_personalization(int tx, int ty);

static void draw_wifi();
static void touch_wifi(int tx, int ty);
static void update_wifi_state();

static void draw_rtc();
static void touch_rtc(int tx, int ty);
static void update_rtc_state();

static bool rtcPendingInitialised = false;
static bool rtcAdvisoryShown      = false;

static void draw_button(int x, int y, int w, int h, const char* label,
                         uint32_t bg, uint32_t fg) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, w, h, 8, bg);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(fg, bg);
    d.drawString(label, x + w / 2, y + h / 2);
}
static bool in_rect(int tx, int ty, int x, int y, int w, int h) {
    return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

static const char* NVS_NS = "p4os";

void settings_load() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return;

    os.accentColor   = prefs.getUInt("accent", os.accentColor);
    os.wallpaperGrid = prefs.getBool("wallgrid", os.wallpaperGrid);
    os.brightness    = (uint8_t)prefs.getUChar("bright", os.brightness);
    bool autoRotate  = prefs.getBool("autorot", true);

    String ssid = prefs.getString("wssid", "");
    String pass = prefs.getString("wpass", "");
    prefs.end();

    M5.Display.setBrightness(os.brightness);
    rotation_set_auto(autoRotate);

    if (ssid.length() > 0) {

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
}

void settings_save() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return;
    prefs.putUInt("accent", os.accentColor);
    prefs.putBool("wallgrid", os.wallpaperGrid);
    prefs.putUChar("bright", os.brightness);
    prefs.putBool("autorot", rotation_get_auto());
    prefs.end();
}

static void save_wifi_credentials(const char* ssid, const char* pass) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return;
    prefs.putString("wssid", ssid);
    prefs.putString("wpass", pass);
    prefs.end();
}

void settings_app_draw() {
    draw_titlebar_and_tabs();
    navbar_draw("Settings");
    switch (currentTab) {
        case SettingsTab::PERSONALIZATION: draw_personalization(); break;
        case SettingsTab::WIFI:            draw_wifi();            break;
        case SettingsTab::RTC:             draw_rtc();             break;
    }
}

void settings_app_update() {

    switch (currentTab) {
        case SettingsTab::WIFI: update_wifi_state(); break;
        case SettingsTab::RTC:  update_rtc_state();  break;
        default: break;
    }

    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return;

    if (keyboard_is_open()) {
        switch (currentTab) {
            case SettingsTab::WIFI: touch_wifi(t.x, t.y); break;
            default: break;
        }
        return;
    }

    if (navbar_touch(t.x, t.y)) {
        return;
    }

    int tabIdx = touch_tab(t.x, t.y);
    if (tabIdx >= 0) {
        if (tabBtns[tabIdx].tab != currentTab) {

            if (currentTab == SettingsTab::RTC) {
                rtcPendingInitialised = false;
                rtcAdvisoryShown = false;
            }
            currentTab = tabBtns[tabIdx].tab;
            settings_app_draw();
        }
        return;
    }

    switch (currentTab) {
        case SettingsTab::PERSONALIZATION: touch_personalization(t.x, t.y); break;
        case SettingsTab::WIFI:            touch_wifi(t.x, t.y);            break;
        case SettingsTab::RTC:             touch_rtc(t.x, t.y);             break;
    }
}

static void draw_titlebar_and_tabs() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillScreen(COL_SURFACE);

    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString("Settings", W / 2, TITLEBAR_H / 2);

    d.fillRect(0, TITLEBAR_H, W, TAB_BAR_H, COL_CARD);
    d.drawFastHLine(0, TITLEBAR_H + TAB_BAR_H, W, 0x333355U);

    int tabW = W / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        tabBtns[i].x = i * tabW;
        tabBtns[i].y = TITLEBAR_H;
        tabBtns[i].w = tabW;
        tabBtns[i].h = TAB_BAR_H;

        bool active = (tabBtns[i].tab == currentTab);
        uint32_t bg = active ? os.accentColor : COL_CARD;
        uint32_t fg = active ? COL_WHITE : COL_TEXT_DIM;
        d.fillRect(tabBtns[i].x, tabBtns[i].y, tabBtns[i].w, tabBtns[i].h, bg);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(fg, bg);
        d.drawString(tabBtns[i].label, tabBtns[i].x + tabBtns[i].w / 2,
                     tabBtns[i].y + tabBtns[i].h / 2);
    }
}

static int touch_tab(int tx, int ty) {
    for (int i = 0; i < TAB_COUNT; i++) {
        if (in_rect(tx, ty, tabBtns[i].x, tabBtns[i].y, tabBtns[i].w, tabBtns[i].h))
            return i;
    }
    return -1;
}

struct AccentSwatch { uint32_t color; int x, y, w, h; };
static AccentSwatch swatches[] = {
    { 0x1A73E8U, 0,0,0,0 },
    { 0x34A853U, 0,0,0,0 },
    { 0xEA4335U, 0,0,0,0 },
    { 0xFBBC05U, 0,0,0,0 },
    { 0x9C27B0U, 0,0,0,0 },
    { 0x00BCD4U, 0,0,0,0 },
};
static const int SWATCH_COUNT = 6;

static int  brightMinusX, brightMinusY, brightPlusX, brightPlusY, brightRowY;
static int  gridToggleX, gridToggleY, gridToggleW, gridToggleH;
static int  rotToggleX, rotToggleY, rotToggleW, rotToggleH;

static void draw_toggle(int x, int y, int w, int h, bool on, const char* onLabel, const char* offLabel) {
    auto& d = M5.Display;
    uint32_t bg = on ? COL_OK : 0x444455U;
    d.fillRoundRect(x, y, w, h, h / 2, bg);
    int knobD = h - 6;
    int knobX = on ? (x + w - knobD - 3) : (x + 3);
    d.fillCircle(knobX + knobD / 2, y + h / 2, knobD / 2, COL_WHITE);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.drawString(on ? onLabel : offLabel, x + w + 12, y + h / 2);
}

static void draw_personalization() {
    auto& d = M5.Display;
    int W = d.width();
    int x = 60;
    int y = CONTENT_TOP + 36;
    int rowGap = 90;

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);

    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString("Brightness", x, y);
    brightRowY = y;
    char buf[8];
    snprintf(buf, sizeof(buf), "%3d", os.brightness);
    d.fillRect(x + 260, y - 18, 200, 36, COL_SURFACE);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_ACCENT, COL_SURFACE);
    d.drawString(buf, x + 260, y);
    brightMinusX = x + 340; brightMinusY = y - 20;
    draw_button(brightMinusX, brightMinusY, 44, 40, "-", COL_CARD, COL_WHITE);
    brightPlusX = x + 400; brightPlusY = y - 20;
    draw_button(brightPlusX, brightPlusY, 44, 40, "+", COL_CARD, COL_WHITE);

    y += rowGap;
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString("Auto-Rotate", x, y);
    rotToggleX = x + 260; rotToggleY = y - 15; rotToggleW = 64; rotToggleH = 30;
    d.fillRect(rotToggleX, rotToggleY - 4, 260, 40, COL_SURFACE);
    draw_toggle(rotToggleX, rotToggleY, rotToggleW, rotToggleH, rotation_get_auto(), "On", "Off");

    y += rowGap;
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString("Wallpaper Grid", x, y);
    gridToggleX = x + 260; gridToggleY = y - 15; gridToggleW = 64; gridToggleH = 30;
    d.fillRect(gridToggleX, gridToggleY - 4, 260, 40, COL_SURFACE);
    draw_toggle(gridToggleX, gridToggleY, gridToggleW, gridToggleH, os.wallpaperGrid, "On", "Off");

    y += rowGap;
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString("Accent Color", x, y);
    int sx = x + 260, sy = y - 20, sw = 40, sh = 40, gap = 14;
    for (int i = 0; i < SWATCH_COUNT; i++) {
        swatches[i].x = sx + i * (sw + gap);
        swatches[i].y = sy;
        swatches[i].w = sw;
        swatches[i].h = sh;
        d.fillRoundRect(swatches[i].x, swatches[i].y, sw, sh, 10, swatches[i].color);
        if (swatches[i].color == os.accentColor) {
            d.drawRoundRect(swatches[i].x - 3, swatches[i].y - 3, sw + 6, sh + 6, 12, COL_WHITE);
        }
    }

    (void)W;
}

static void touch_personalization(int tx, int ty) {
    bool changed = false;

    if (in_rect(tx, ty, brightMinusX, brightMinusY, 44, 40)) {
        int v = (int)os.brightness - 15;
        os.brightness = (uint8_t)(v < 10 ? 10 : v);
        M5.Display.setBrightness(os.brightness);
        changed = true;
    } else if (in_rect(tx, ty, brightPlusX, brightPlusY, 44, 40)) {
        int v = (int)os.brightness + 15;
        os.brightness = (uint8_t)(v > 255 ? 255 : v);
        M5.Display.setBrightness(os.brightness);
        changed = true;
    } else if (in_rect(tx, ty, rotToggleX, rotToggleY, rotToggleW + 60, rotToggleH)) {
        rotation_set_auto(!rotation_get_auto());
        changed = true;
    } else if (in_rect(tx, ty, gridToggleX, gridToggleY, gridToggleW + 60, gridToggleH)) {
        os.wallpaperGrid = !os.wallpaperGrid;
        changed = true;
    } else {
        for (int i = 0; i < SWATCH_COUNT; i++) {
            if (in_rect(tx, ty, swatches[i].x, swatches[i].y, swatches[i].w, swatches[i].h)) {
                os.accentColor = swatches[i].color;
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        settings_save();
        settings_app_draw();
    }
}

enum class WifiUiState { IDLE, SCANNING, LIST, KEYBOARD, CONNECTING, FAILED };
static WifiUiState wifiState = WifiUiState::IDLE;

static int      wifiNetworkCount = 0;
static char     pendingSSID[33]  = "";
static bool     pendingOpen      = false;
static char     passwordBuf[65]  = "";
static uint32_t wifiConnectStartMs = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static char     wifiErrorMsg[64] = "";

static int bannerBtnX, bannerBtnY, bannerBtnW, bannerBtnH;

struct WifiRow { int x, y, w, h; int netIdx; };
static WifiRow wifiRows[10];
static int wifiRowCount = 0;

static void wifi_reset_scan_ui() {
    wifiState = WifiUiState::IDLE;
}

static void start_scan() {
    WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(true );
    wifiState = WifiUiState::SCANNING;
    settings_app_draw();
}

static void begin_connect(const char* ssid, const char* pass) {
    strncpy(pendingSSID, ssid, sizeof(pendingSSID) - 1);
    pendingSSID[sizeof(pendingSSID) - 1] = '\0';
    WiFi.begin(ssid, pass);
    wifiConnectStartMs = millis();
    wifiState = WifiUiState::CONNECTING;
    settings_app_draw();
}

static void draw_wifi_banner() {
    auto& d = M5.Display;
    int W = d.width();
    int y = CONTENT_TOP + 16;
    d.fillRect(30, y, W - 60, 56, COL_CARD);

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);

    char line[96];
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(line, sizeof(line), "Connected: %s  (%s)",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        d.setTextColor(COL_OK, COL_CARD);
    } else if (wifiState == WifiUiState::CONNECTING) {
        snprintf(line, sizeof(line), "Connecting to %s ...", pendingSSID);
        d.setTextColor(COL_ACCENT, COL_CARD);
    } else if (wifiState == WifiUiState::SCANNING) {
        snprintf(line, sizeof(line), "Scanning for networks...");
        d.setTextColor(COL_TEXT_DIM, COL_CARD);
    } else {
        snprintf(line, sizeof(line), "Not connected");
        d.setTextColor(COL_TEXT_DIM, COL_CARD);
    }
    d.drawString(line, 50, y + 28);

    bannerBtnW = 160; bannerBtnH = 40;
    bannerBtnX = W - 30 - bannerBtnW; bannerBtnY = y + 8;

    if (WiFi.status() == WL_CONNECTED) {
        draw_button(bannerBtnX, bannerBtnY, bannerBtnW, bannerBtnH, "Disconnect", COL_DANGER, COL_WHITE);
    } else if (wifiState == WifiUiState::SCANNING || wifiState == WifiUiState::CONNECTING) {
        draw_button(bannerBtnX, bannerBtnY, bannerBtnW, bannerBtnH, "Cancel", COL_CARD, COL_TEXT_DIM);
    } else {
        draw_button(bannerBtnX, bannerBtnY, bannerBtnW, bannerBtnH, "Scan", os.accentColor, COL_WHITE);
    }
}

static const char* enc_is_open(wifi_auth_mode_t enc) {
    return (enc == WIFI_AUTH_OPEN) ? "open" : "secured";
}

static void draw_wifi_list() {
    auto& d = M5.Display;
    int W = d.width();
    int top = CONTENT_TOP + 90;
    int rowH = 50;
    int maxRows = 8;

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font2);

    wifiRowCount = (wifiNetworkCount < maxRows) ? wifiNetworkCount : maxRows;
    if (wifiRowCount <= 0) {
        d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
        d.setTextDatum(middle_center);
        d.drawString("No networks found. Try Scan again.", W / 2, top + 40);
        return;
    }

    for (int i = 0; i < wifiRowCount; i++) {
        int y = top + i * rowH;
        wifiRows[i] = { 40, y, W - 80, rowH - 6, i };

        uint32_t rowBg = (i % 2 == 0) ? COL_CARD : COL_SURFACE;
        d.fillRect(40, y, W - 80, rowH - 6, rowBg);

        String ssid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        wifi_auth_mode_t enc = WiFi.encryptionType(i);

        int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
        char barsStr[6] = "";
        for (int b = 0; b < 4; b++) strcat(barsStr, (b < bars) ? "|" : ".");

        d.setTextDatum(middle_left);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_WHITE, rowBg);
        char label[80];
        snprintf(label, sizeof(label), "%s  %s",
                 (enc == WIFI_AUTH_OPEN) ? "" : "[locked] ",
                 ssid.length() ? ssid.c_str() : "(hidden network)");
        d.drawString(label, 60, y + rowH / 2 - 3);

        d.setTextDatum(middle_right);
        d.setTextColor(COL_TEXT_DIM, rowBg);
        char rssiLabel[32];
        snprintf(rssiLabel, sizeof(rssiLabel), "%s  %ddBm", barsStr, (int)rssi);
        d.drawString(rssiLabel, W - 60, y + rowH / 2 - 3);
    }

    if (wifiNetworkCount > maxRows) {
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
        char more[32];
        snprintf(more, sizeof(more), "+%d more (rescan to refresh)", wifiNetworkCount - maxRows);
        d.drawString(more, W / 2, top + maxRows * rowH + 16);
    }
}

static void draw_wifi() {
    draw_wifi_banner();

    switch (wifiState) {
        case WifiUiState::IDLE: {
            auto& d = M5.Display;
            int W = d.width();
            d.setTextDatum(middle_center);
            d.setFont(&fonts::Font2);
            d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
            d.drawString("Tap Scan to find nearby Wi-Fi networks.", W / 2, CONTENT_TOP + 150);
            break;
        }
        case WifiUiState::SCANNING:
            break;
        case WifiUiState::LIST:
            draw_wifi_list();
            break;
        case WifiUiState::KEYBOARD:

            keyboard_draw();
            if (wifiErrorMsg[0]) {
                auto& d = M5.Display;
                d.setTextDatum(middle_center);
                d.setFont(&fonts::Font2);
                d.setTextColor(COL_DANGER, COL_SURFACE);
                d.drawString(wifiErrorMsg, d.width() / 2, CONTENT_TOP + 70);
            }
            break;
        case WifiUiState::CONNECTING:
            break;
        case WifiUiState::FAILED: {
            auto& d = M5.Display;
            int W = d.width();
            d.setTextDatum(middle_center);
            d.setFont(&fonts::Font2);
            d.setTextColor(COL_DANGER, COL_SURFACE);
            char msg[96];
            snprintf(msg, sizeof(msg), "Could not connect to \"%s\". Check the password and try again.", pendingSSID);
            d.drawString(msg, W / 2, CONTENT_TOP + 150);
            break;
        }
    }
}

static void touch_wifi(int tx, int ty) {

    if (in_rect(tx, ty, bannerBtnX, bannerBtnY, bannerBtnW, bannerBtnH)) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(true, true);
            save_wifi_credentials("", "");
            wifiState = WifiUiState::IDLE;
            settings_app_draw();
        } else if (wifiState == WifiUiState::SCANNING) {
            WiFi.scanDelete();
            wifiState = WifiUiState::IDLE;
            settings_app_draw();
        } else if (wifiState == WifiUiState::CONNECTING) {
            WiFi.disconnect(true);
            wifiState = WifiUiState::LIST;
            settings_app_draw();
        } else {
            start_scan();
        }
        return;
    }

    switch (wifiState) {
        case WifiUiState::LIST: {
            for (int i = 0; i < wifiRowCount; i++) {
                if (in_rect(tx, ty, wifiRows[i].x, wifiRows[i].y, wifiRows[i].w, wifiRows[i].h)) {
                    int netIdx = wifiRows[i].netIdx;
                    String ssid = WiFi.SSID(netIdx);
                    wifi_auth_mode_t enc = WiFi.encryptionType(netIdx);
                    strncpy(pendingSSID, ssid.c_str(), sizeof(pendingSSID) - 1);
                    pendingSSID[sizeof(pendingSSID) - 1] = '\0';
                    pendingOpen = (enc == WIFI_AUTH_OPEN);
                    wifiErrorMsg[0] = '\0';

                    if (pendingOpen) {
                        begin_connect(pendingSSID, "");
                    } else {
                        passwordBuf[0] = '\0';
                        wifiErrorMsg[0] = '\0';

                        char fieldLabel[64];
                        snprintf(fieldLabel, sizeof(fieldLabel), "Password for \"%s\":", pendingSSID);
                        keyboard_open(passwordBuf, (int)sizeof(passwordBuf),
                                      true, "Connect", fieldLabel);
                        wifiState = WifiUiState::KEYBOARD;
                        settings_app_draw();
                    }
                    return;
                }
            }
            break;
        }
        case WifiUiState::KEYBOARD: {
            KeyboardResult kr = keyboard_touch(tx, ty);
            if (!kr.handled) break;

            if (kr.cancelled) {
                keyboard_close();
                wifiState = WifiUiState::LIST;
                settings_app_draw();
            } else if (kr.submitted) {

                int passwordLen = (int)strnlen(passwordBuf, sizeof(passwordBuf));
                if (passwordLen == 0) {
                    strncpy(wifiErrorMsg, "Enter a password first.", sizeof(wifiErrorMsg) - 1);
                    settings_app_draw();
                } else {
                    wifiErrorMsg[0] = '\0';
                    keyboard_close();
                    begin_connect(pendingSSID, passwordBuf);
                }
            }

            break;
        }
        case WifiUiState::FAILED: {

            wifiState = (wifiNetworkCount > 0) ? WifiUiState::LIST : WifiUiState::IDLE;
            settings_app_draw();
            break;
        }
        default:
            break;
    }
}

// polls the async wifi scan / connect calls and advances the UI state once they resolve
static void update_wifi_state() {
    if (wifiState == WifiUiState::SCANNING) {
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            return;
        } else if (n == WIFI_SCAN_FAILED) {
            wifiState = WifiUiState::IDLE;
            settings_app_draw();
        } else {
            wifiNetworkCount = n;
            wifiState = WifiUiState::LIST;
            settings_app_draw();
        }
        return;
    }

    if (wifiState == WifiUiState::CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            save_wifi_credentials(pendingSSID, pendingOpen ? "" : passwordBuf);
            wifiState = WifiUiState::LIST;
            settings_app_draw();
        } else if (millis() - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
            WiFi.disconnect(true);
            wifiState = WifiUiState::FAILED;
            settings_app_draw();
        }
    }
}

static uint32_t lastRtcTickMs = 0;
static char     lastRtcStr[16] = "";
static int      pendingHour = 0, pendingMinute = 0;

static char     rtcToast[48] = "";
static uint32_t rtcToastUntilMs = 0;

static int hourMinusX, hourMinusY, hourPlusX, hourPlusY, hourRowY;
static int minMinusX, minMinusY, minPlusX, minPlusY;
static int setTimeBtnX, setTimeBtnY, setTimeBtnW, setTimeBtnH;
static int advisoryConfirmX, advisoryConfirmY, advisoryCancelX, advisoryCancelY, advisoryBtnW, advisoryBtnH;

static void rtc_sync_pending_from_hw() {
    if (rtcPendingInitialised) return;
    m5::rtc_time_t t;
    if (M5.Rtc.getTime(&t)) {
        pendingHour   = t.hours;
        pendingMinute = t.minutes;
        rtcPendingInitialised = true;
    }
}

static void draw_rtc_clock() {
    auto& d = M5.Display;
    int W = d.width();

    m5::rtc_time_t t;
    char timeStr[16];
    if (M5.Rtc.getTime(&t)) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", t.hours, t.minutes, t.seconds);
    } else {
        snprintf(timeStr, sizeof(timeStr), "--:--:--");
    }

    d.fillRect(0, CONTENT_TOP, W, 110, COL_SURFACE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font7);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString(timeStr, W / 2, CONTENT_TOP + 55);
}

static void draw_rtc_stepper_row(int x, int y, const char* label, int value,
                                  int& minusX, int& minusY, int& plusX, int& plusY) {
    auto& d = M5.Display;
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString(label, x, y);

    minusX = x + 140; minusY = y - 20;
    draw_button(minusX, minusY, 44, 40, "-", COL_CARD, COL_WHITE);

    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", value);
    d.fillRect(x + 195, y - 18, 60, 36, COL_SURFACE);
    d.setTextColor(COL_ACCENT, COL_SURFACE);
    d.setFont(&fonts::Font4);
    d.setTextDatum(middle_center);
    d.drawString(buf, x + 225, y);

    plusX = x + 260; plusY = y - 20;
    draw_button(plusX, plusY, 44, 40, "+", COL_CARD, COL_WHITE);
}

static void draw_rtc_advisory() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    int cw = 720, ch = 260;
    int cx = (W - cw) / 2, cy = (H - ch) / 2;

    d.fillRoundRect(cx, cy, cw, ch, 16, COL_CARD);
    d.drawRoundRect(cx, cy, cw, ch, 16, os.accentColor);

    d.setTextDatum(top_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_CARD);
    d.drawString("Confirm Time Set", W / 2, cy + 20);

    char msg[80];
    snprintf(msg, sizeof(msg), "Only hit Set Time when real time is at %02d:%02d:00",
             pendingHour, pendingMinute);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_CARD);

    char line1[80], line2[80];
    snprintf(line1, sizeof(line1), "Only hit Set Time when real time is");
    snprintf(line2, sizeof(line2), "at %02d:%02d:00", pendingHour, pendingMinute);
    d.drawString(line1, W / 2, cy + 80);
    d.drawString(line2, W / 2, cy + 108);

    advisoryBtnW = 220; advisoryBtnH = 50;
    advisoryCancelX = cx + 60;          advisoryCancelY = cy + ch - 74;
    advisoryConfirmX = cx + cw - 60 - advisoryBtnW; advisoryConfirmY = cy + ch - 74;

    draw_button(advisoryCancelX, advisoryCancelY, advisoryBtnW, advisoryBtnH, "Cancel", 0x444455U, COL_WHITE);
    draw_button(advisoryConfirmX, advisoryConfirmY, advisoryBtnW, advisoryBtnH, "Set Time", COL_OK, COL_WHITE);

    (void)msg;
}

static void draw_rtc() {
    auto& d = M5.Display;
    int W = d.width();

    rtc_sync_pending_from_hw();
    draw_rtc_clock();

    int x = 60;
    int y = CONTENT_TOP + 190;
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.drawString("Set a new time:", x, y - 34);

    hourRowY = y;
    draw_rtc_stepper_row(x, hourRowY, "Hour", pendingHour, hourMinusX, hourMinusY, hourPlusX, hourPlusY);
    draw_rtc_stepper_row(x, hourRowY + 80, "Minute", pendingMinute, minMinusX, minMinusY, minPlusX, minPlusY);

    setTimeBtnW = 260; setTimeBtnH = 54;
    setTimeBtnX = x; setTimeBtnY = hourRowY + 170;
    draw_button(setTimeBtnX, setTimeBtnY, setTimeBtnW, setTimeBtnH, "SET TIME", COL_DANGER, COL_WHITE);

    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
    d.drawString("Seconds are always set to :00 on confirm.", setTimeBtnX + setTimeBtnW + 24, setTimeBtnY + 18);

    (void)W;
    lastRtcTickMs = millis();
    lastRtcStr[0] = '\0';

    if (rtcAdvisoryShown) draw_rtc_advisory();

    if (rtcToast[0] && millis() < rtcToastUntilMs) {
        d.setTextDatum(bottom_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_OK, COL_SURFACE);
        d.drawString(rtcToast, W / 2, 700);
    }
}

static void touch_rtc(int tx, int ty) {
    if (rtcAdvisoryShown) {
        if (in_rect(tx, ty, advisoryConfirmX, advisoryConfirmY, advisoryBtnW, advisoryBtnH)) {
            m5::rtc_time_t t;
            t.hours   = pendingHour;
            t.minutes = pendingMinute;
            t.seconds = 0;
            M5.Rtc.setTime(&t);

            rtcAdvisoryShown = false;
            rtcPendingInitialised = false;
            snprintf(rtcToast, sizeof(rtcToast), "Time set to %02d:%02d:00", pendingHour, pendingMinute);
            rtcToastUntilMs = millis() + 2500;
            settings_app_draw();
        } else if (in_rect(tx, ty, advisoryCancelX, advisoryCancelY, advisoryBtnW, advisoryBtnH)) {
            rtcAdvisoryShown = false;
            settings_app_draw();
        }
        return;
    }

    // add (period - 1) instead of subtracting 1 to wrap around 0 without going negative
    if (in_rect(tx, ty, hourMinusX, hourMinusY, 44, 40)) {
        pendingHour = (pendingHour + 23) % 24; settings_app_draw();
    } else if (in_rect(tx, ty, hourPlusX, hourPlusY, 44, 40)) {
        pendingHour = (pendingHour + 1) % 24; settings_app_draw();
    } else if (in_rect(tx, ty, minMinusX, minMinusY, 44, 40)) {
        pendingMinute = (pendingMinute + 59) % 60; settings_app_draw();
    } else if (in_rect(tx, ty, minPlusX, minPlusY, 44, 40)) {
        pendingMinute = (pendingMinute + 1) % 60; settings_app_draw();
    } else if (in_rect(tx, ty, setTimeBtnX, setTimeBtnY, setTimeBtnW, setTimeBtnH)) {
        rtcAdvisoryShown = true;
        settings_app_draw();
    }
}

static void update_rtc_state() {
    uint32_t now = millis();
    if (rtcAdvisoryShown) return;

    if (now - lastRtcTickMs >= 1000) {
        lastRtcTickMs = now;
        draw_rtc_clock();

        if (rtcToast[0] && now >= rtcToastUntilMs) {
            rtcToast[0] = '\0';
            auto& d = M5.Display;
            d.fillRect(0, 690, d.width(), 24, COL_SURFACE);
        }
    }
}

