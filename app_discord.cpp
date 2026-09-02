#include <M5Unified.h>
#include "p4_os.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <ctime>
#include "discord_gateway.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DC_BLURPLE   0x5865F2U
#define DC_DARK_BG   0x313338U
#define DC_DARKER_BG 0x2B2D31U
#define DC_SURFACE   0x383A40U
#define DC_TEXT      0xDBDEE1U
#define DC_MUTED     0x949BA4U
#define DC_GREEN     0x23A559U
#define DC_RED       0xF23F43U

static const int TITLEBAR_H = 56;

#define DC_AVATAR_DIAM 96

static const char* DC_NVS_NS = "discord";

enum class DcScreen { LOGIN, LOADING, DM_LIST, DM_CHANNEL, PROFILE, ERROR };
static DcScreen dcScreen = DcScreen::LOGIN;

static bool dcAutoLogin = false;

static char emailBuf[128]    = "";
static char passwordBuf[128] = "";

enum class ActiveField { NONE, EMAIL, PASSWORD };
static ActiveField activeField = ActiveField::NONE;

struct Rect { int x, y, w, h; };
static Rect emailRect, passRect, loginBtnRect;

static char authToken[512]       = "";

static char dcUsername[128]      = "";
static char dcGlobalName[128]    = "";
static char dcDiscriminator[8]   = "";
static char dcEmail[128]         = "";
static char dcId[32]             = "";
static char dcAvatarHash[64]     = "";
static char dcErrorMsg[256]      = "";

static uint8_t* avatarPngBuf  = nullptr;
static size_t   avatarPngSize = 0;
static int      avatarPngW    = 0;
static int      avatarPngH    = 0;
static bool     avatarReady   = false;

static uint32_t loadingStartMs = 0;
static uint8_t  spinnerFrame   = 0;

static uint32_t lastTouchMs        = 0;
static const uint32_t TOUCH_DEBOUNCE = 300;

static void draw_login();
static void update_login();
static void draw_loading();
static void update_loading();
static void draw_profile();
static void update_profile();
static void draw_dm_list();
static void update_dm_list();
static void draw_dm_channel();
static void update_dm_channel();
static void open_dm_channel(int dmIndex);
static void draw_error();
static void update_error();
static void do_login();
static void do_autologin();
static void fetch_avatar();
static void nvs_save();
static void nvs_load();
static void nvs_clear();
static void draw_titlebar(const char* subtitle);
static bool in_rect(int tx, int ty, const Rect& r);
static void draw_field(const Rect& r, const char* label, const char* value,
                        bool obscure, bool focused, uint32_t accent);
static void draw_avatar(int cx, int cy, int r);
static void draw_avatar_buf(int cx, int cy, int radius,
                             uint8_t* pngBuf, size_t pngSize, int pngW, int pngH,
                             bool ready, char fallbackInitial);

void discord_app_draw() {
    M5.Display.fillScreen(DC_DARK_BG);
    switch (dcScreen) {
        case DcScreen::LOGIN:   draw_login();   break;
        case DcScreen::LOADING: draw_loading(); break;
        case DcScreen::DM_LIST:    draw_dm_list();    break;
        case DcScreen::DM_CHANNEL: draw_dm_channel(); break;
        case DcScreen::PROFILE:    draw_profile();    break;
        case DcScreen::ERROR:      draw_error();      break;
    }
}

void discord_app_update() {
    switch (dcScreen) {
        case DcScreen::LOGIN:      update_login();      break;
        case DcScreen::LOADING:    update_loading();    break;
        case DcScreen::DM_LIST:    update_dm_list();    break;
        case DcScreen::DM_CHANNEL: update_dm_channel(); break;
        case DcScreen::PROFILE:    update_profile();    break;
        case DcScreen::ERROR:      update_error();      break;
    }
}

void discord_app_init() {

    avatarReady  = false;
    avatarPngW   = 0;
    avatarPngH   = 0;
    if (avatarPngBuf) {
        heap_caps_free(avatarPngBuf);
        avatarPngBuf  = nullptr;
        avatarPngSize = 0;
    }

    nvs_load();

    if (authToken[0] != '\0') {

        dcAutoLogin    = true;
        dcScreen       = DcScreen::LOADING;
        loadingStartMs = millis();
        spinnerFrame   = 0;
    } else {
        dcScreen    = DcScreen::LOGIN;
        activeField = ActiveField::NONE;
    }
}

void discord_app_exit() {
    dc_gateway_stop();
}

static void nvs_save() {
    Preferences prefs;
    if (!prefs.begin(DC_NVS_NS, false)) return;
    prefs.putString("token", authToken);
    prefs.putString("email", emailBuf);
    prefs.end();
}

static void nvs_load() {
    Preferences prefs;
    if (!prefs.begin(DC_NVS_NS, true)) return;
    String tok   = prefs.getString("token", "");
    String email = prefs.getString("email", "");
    prefs.end();
    snprintf(authToken, sizeof(authToken), "%s", tok.c_str());
    snprintf(emailBuf,  sizeof(emailBuf),  "%s", email.c_str());
}

static void nvs_clear() {
    Preferences prefs;
    if (!prefs.begin(DC_NVS_NS, false)) return;
    prefs.remove("token");
    prefs.remove("email");
    prefs.end();
}

static bool in_rect(int tx, int ty, const Rect& r) {
    return tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h;
}

static void draw_titlebar(const char* subtitle) {
    auto& d = M5.Display;
    int W = d.width();
    d.fillRect(0, 0, W, TITLEBAR_H, DC_DARKER_BG);
    d.drawFastHLine(0, TITLEBAR_H, W, DC_SURFACE);

    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_BLURPLE, DC_DARKER_BG);
    d.drawString("Discord", 20, TITLEBAR_H / 2);

    if (subtitle && subtitle[0]) {
        d.setTextDatum(middle_right);
        d.setFont(&fonts::Font2);
        d.setTextColor(DC_MUTED, DC_DARKER_BG);
        d.drawString(subtitle, W - 20, TITLEBAR_H / 2);
    }
}

static void draw_field(const Rect& r, const char* label, const char* value,
                        bool obscure, bool focused, uint32_t accent) {
    auto& d = M5.Display;

    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(focused ? accent : DC_MUTED, DC_DARK_BG);
    d.drawString(label, r.x, r.y - 26);

    uint32_t boxBg = DC_DARKER_BG;
    d.fillRoundRect(r.x, r.y, r.w, r.h, 6, boxBg);
    if (focused) {
        d.drawRoundRect(r.x,     r.y,     r.w,     r.h,     6, accent);
        d.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, accent);
    } else {
        d.drawRoundRect(r.x, r.y, r.w, r.h, 6, DC_SURFACE);
    }

    char shown[128];
    if (obscure) {
        int n = (int)strnlen(value, 126);
        for (int i = 0; i < n; i++) shown[i] = '*';
        shown[n] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", value);
    }
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, boxBg);
    d.drawString(shown, r.x + 16, r.y + r.h / 2);

    if (focused) {
        int textW = (int)strnlen(shown, 127) * 18;
        int curX  = r.x + 16 + textW;
        if (curX > r.x + r.w - 8) curX = r.x + r.w - 8;
        d.fillRect(curX, r.y + 12, 2, r.h - 24, accent);
    }
}

static void draw_avatar_buf(int cx, int cy, int radius,
                             uint8_t* pngBuf, size_t pngSize, int pngW, int pngH,
                             bool ready, char fallbackInitial) {
    auto& d = M5.Display;

    if (ready && pngBuf && pngSize > 0 && pngW > 0) {
        LGFX_Sprite tmp;
        tmp.setPsram(true);
        tmp.setColorDepth(24);

        if (tmp.createSprite(pngW, pngH)) {

            tmp.setSwapBytes(d.getSwapBytes());
            tmp.fillSprite(0x313338);

            tmp.drawPng(pngBuf, pngSize, 0, 0);

            d.fillCircle(cx, cy, radius, DC_DARK_BG);

            int imgCX = pngW / 2;
            int imgCY = pngH / 2;
            int sampleR = radius - 1;
            int r2      = sampleR * sampleR;
            int diam    = radius * 2;

            uint16_t rowBuf[144];
            int bufCap = (int)(sizeof(rowBuf) / sizeof(rowBuf[0]));

            for (int row = 0; row < diam; row++) {
                int dy  = row - radius;
                int dx2 = r2 - dy * dy;
                if (dx2 < 0) continue;

                int dx  = (int)sqrtf((float)dx2);
                int x0  = radius - dx;
                int x1  = radius + dx;
                int len = x1 - x0 + 1;
                if (len > bufCap) len = bufCap;

                int sprY = imgCY - radius + row;
                int sprX = imgCX - radius + x0;

                if (sprY < 0 || sprY >= pngH) continue;

                int clampX0  = (sprX < 0)            ? 0          : sprX;
                int clampX1  = (sprX + len > pngW) ? pngW : sprX + len;
                int clampLen = clampX1 - clampX0;
                if (clampLen <= 0) continue;

                int bufOff  = clampX0 - sprX;
                int dispX   = cx - radius + x0 + bufOff;

                tmp.readRect(clampX0, sprY, clampLen, 1, rowBuf);
                d.pushImage(dispX, cy - radius + row, clampLen, 1, rowBuf);
            }

            tmp.deleteSprite();
            return;
        }
        tmp.deleteSprite();

    }

    d.fillCircle(cx, cy, radius, DC_BLURPLE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8);
    d.setTextColor(COL_WHITE, DC_BLURPLE);
    char initial[2] = { fallbackInitial ? (char)toupper((unsigned char)fallbackInitial) : '?', '\0' };
    d.drawString(initial, cx, cy);
}

static void draw_avatar(int cx, int cy, int radius) {
    draw_avatar_buf(cx, cy, radius, avatarPngBuf, avatarPngSize,
                     avatarPngW, avatarPngH, avatarReady, dcUsername[0]);
}

static void draw_login() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    draw_titlebar("Sign in");

    int contentH = H - TITLEBAR_H - navbar_height();
    int cardW    = (W > 900) ? 640 : W - 80;
    int cardH    = 460;
    int cardX    = (W - cardW) / 2;
    int cardY    = TITLEBAR_H + (contentH - cardH) / 2;

    d.fillRoundRect(cardX, cardY, cardW, cardH, 16, DC_DARKER_BG);

    d.setTextDatum(top_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_DARKER_BG);
    d.drawString("Welcome back!", W / 2, cardY + 28);

    d.setFont(&fonts::Font2);
    d.setTextColor(DC_MUTED, DC_DARKER_BG);
    d.drawString("Sign in to continue to Discord", W / 2, cardY + 68);

    int fieldW = cardW - 80;
    int fieldH = 70;
    int fieldX = cardX + 40;

    emailRect = { fieldX, cardY + 112, fieldW, fieldH };
    draw_field(emailRect, "EMAIL OR PHONE NUMBER", emailBuf, false,
                activeField == ActiveField::EMAIL, DC_BLURPLE);

    passRect = { fieldX, cardY + 230, fieldW, fieldH };
    draw_field(passRect, "PASSWORD", passwordBuf, true,
                activeField == ActiveField::PASSWORD, DC_BLURPLE);

    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(DC_BLURPLE, DC_DARKER_BG);
    d.drawString("Forgot your password?", fieldX, cardY + 314);

    int btnH = 64;
    loginBtnRect = { fieldX, cardY + 360, fieldW, btnH };
    d.fillRoundRect(loginBtnRect.x, loginBtnRect.y, loginBtnRect.w, loginBtnRect.h, 6, DC_BLURPLE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, DC_BLURPLE);
    d.drawString("Log In", W / 2, loginBtnRect.y + btnH / 2);

    if (keyboard_is_open()) {
        keyboard_draw();
    } else {
        navbar_draw("Discord");
    }
}

static void update_login() {
    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return;

    uint32_t now = millis();
    if (now - lastTouchMs < TOUCH_DEBOUNCE) return;
    lastTouchMs = now;

    int tx = t.x, ty = t.y;

    if (keyboard_is_open()) {
        auto kr = keyboard_touch(tx, ty);
        if (kr.handled) {
            os.dirty = true;
            if (kr.submitted || kr.cancelled) {
                keyboard_close();
                activeField = ActiveField::NONE;
                os.dirty = true;
            }
        }
        return;
    }

    if (navbar_touch(tx, ty)) return;

    if (in_rect(tx, ty, emailRect)) {
        activeField = ActiveField::EMAIL;
        keyboard_open(emailBuf, sizeof(emailBuf), false, "Done", "Email or Phone");
        os.dirty = true;
        return;
    }
    if (in_rect(tx, ty, passRect)) {
        activeField = ActiveField::PASSWORD;
        keyboard_open(passwordBuf, sizeof(passwordBuf), true, "Done", "Password");
        os.dirty = true;
        return;
    }
    if (in_rect(tx, ty, loginBtnRect)) {
        if (emailBuf[0] == '\0') {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg), "Please enter your email or phone number.");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        if (passwordBuf[0] == '\0') {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg), "Please enter your password.");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        dcAutoLogin    = false;
        dcScreen       = DcScreen::LOADING;
        loadingStartMs = millis();
        spinnerFrame   = 0;
        os.dirty = true;
        return;
    }

    activeField = ActiveField::NONE;
    os.dirty = true;
}

static const char* kSpinnerFrames[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

static void draw_loading() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    draw_titlebar(dcAutoLogin ? "Resuming session..." : "Signing in...");

    int cy = (H - navbar_height()) / 2;
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8);
    d.setTextColor(DC_BLURPLE, DC_DARK_BG);
    d.drawString(kSpinnerFrames[spinnerFrame % 8], W / 2, cy - 40);

    d.setFont(&fonts::Font4);
    d.setTextColor(DC_MUTED, DC_DARK_BG);
    d.drawString(dcAutoLogin ? "Checking saved session..." : "Connecting to Discord...",
                 W / 2, cy + 60);

    navbar_draw("Discord");
}

static void update_loading() {
    uint32_t now = millis();
    if (now - loadingStartMs > 120) {
        loadingStartMs = now;
        spinnerFrame++;
        os.dirty = true;
    }
    if (spinnerFrame == 1) {
        if (dcAutoLogin) do_autologin();
        else             do_login();
    }
}

static bool parse_me_response(const String& payload) {
    DynamicJsonDocument doc(4096);
    auto err = deserializeJson(doc, payload);
    if (err) {
        snprintf(dcErrorMsg, sizeof(dcErrorMsg),
                 "Couldn't parse user info.\n(%s)", err.c_str());
        return false;
    }
    snprintf(dcUsername,      sizeof(dcUsername),      "%s", doc["username"]      | "");
    snprintf(dcGlobalName,    sizeof(dcGlobalName),    "%s", doc["global_name"]   | "");
    snprintf(dcDiscriminator, sizeof(dcDiscriminator), "%s", doc["discriminator"] | "0");
    snprintf(dcEmail,         sizeof(dcEmail),         "%s", doc["email"]         | "");
    snprintf(dcId,            sizeof(dcId),            "%s", doc["id"]            | "");
    snprintf(dcAvatarHash,    sizeof(dcAvatarHash),    "%s", doc["avatar"]        | "");
    return true;
}

// logs in via Discord's (unofficial) user login endpoint used by the official
// client itself, since bots can't read DMs — this app authenticates as a normal user
static void do_login() {
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(dcErrorMsg, sizeof(dcErrorMsg),
                 "No Wi-Fi connection.\nConnect in Settings first.");
        dcScreen = DcScreen::ERROR; os.dirty = true; return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://discord.com/api/v9/auth/login");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent",   "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
    http.setTimeout(15000);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"login\":\"%s\",\"password\":\"%s\","
             "\"undelete\":false,\"login_source\":null,\"gift_code_sku_id\":null}",
             emailBuf, passwordBuf);

    int code = http.POST(body);
    String payload = http.getString();
    http.end();

    if (code <= 0) {
        snprintf(dcErrorMsg, sizeof(dcErrorMsg), "Network error (%d).\nCheck your connection.", code);
        dcScreen = DcScreen::ERROR; os.dirty = true; return;
    }

    {
        DynamicJsonDocument doc(4096);
        if (deserializeJson(doc, payload)) {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg), "Bad response from Discord.");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        if (doc.containsKey("mfa") && doc["mfa"].as<bool>()) {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg),
                     "Two-factor authentication required.\n"
                     "P4-Tab OS does not support MFA yet.\n"
                     "Disable 2FA temporarily to log in.");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        if (doc.containsKey("message") && !doc.containsKey("token")) {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg), "%s", doc["message"] | "Unknown error");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        const char* tok = doc["token"] | "";
        if (tok[0] == '\0') {
            snprintf(dcErrorMsg, sizeof(dcErrorMsg), "No token returned.\nTry again.");
            dcScreen = DcScreen::ERROR; os.dirty = true; return;
        }
        snprintf(authToken, sizeof(authToken), "%s", tok);

        const char* uid = doc["user_id"] | "";
        snprintf(dcId, sizeof(dcId), "%s", uid);
    }

    http.begin(client, "https://discord.com/api/v9/users/@me");
    http.addHeader("Authorization", authToken);
    http.addHeader("User-Agent",    "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
    http.setTimeout(10000);
    int code2 = http.GET();
    String payload2 = http.getString();
    http.end();

    if (code2 != 200) {
        snprintf(dcErrorMsg, sizeof(dcErrorMsg),
                 "Logged in but couldn't fetch profile (HTTP %d).", code2);
        dcScreen = DcScreen::ERROR; os.dirty = true; return;
    }
    if (!parse_me_response(payload2)) {
        dcScreen = DcScreen::ERROR; os.dirty = true; return;
    }

    nvs_save();

    // gateway and the plain HTTP avatar fetch both want the wifi/TLS stack to
    // themselves for a moment, so stop the websocket, grab the avatar, then reconnect
    dc_gateway_start(authToken);

    dc_gateway_stop();
    fetch_avatar();
    dc_gateway_start(authToken);

    dcScreen = DcScreen::DM_LIST;
    os.dirty = true;
}

// re-validates a token saved from a previous session instead of asking for
// email/password again
static void do_autologin() {
    if (WiFi.status() != WL_CONNECTED) {

        dcScreen = DcScreen::LOGIN;
        os.dirty = true;
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://discord.com/api/v9/users/@me");
    http.addHeader("Authorization", authToken);
    http.addHeader("User-Agent",    "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
    http.setTimeout(10000);
    int code = http.GET();
    String payload = http.getString();
    http.end();

    if (code == 401) {
        // saved token is no longer valid — forget it and fall back to manual login
        nvs_clear();
        memset(authToken, 0, sizeof(authToken));
        dcScreen = DcScreen::LOGIN;
        os.dirty = true;
        return;
    }
    if (code != 200) {
        snprintf(dcErrorMsg, sizeof(dcErrorMsg),
                 "Couldn't resume session (HTTP %d).\nTry logging in again.", code);
        nvs_clear();
        memset(authToken, 0, sizeof(authToken));
        dcScreen = DcScreen::ERROR;
        os.dirty = true;
        return;
    }
    if (!parse_me_response(payload)) {
        dcScreen = DcScreen::ERROR; os.dirty = true; return;
    }

    dc_gateway_start(authToken);

    dc_gateway_stop();
    fetch_avatar();
    dc_gateway_start(authToken);

    dcScreen = DcScreen::DM_LIST;
    os.dirty = true;
}

static void fetch_avatar() {
    avatarReady = false;
    if (avatarPngBuf) {
        heap_caps_free(avatarPngBuf);
        avatarPngBuf  = nullptr;
        avatarPngSize = 0;
    }

    if (dcAvatarHash[0] == '\0') return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://cdn.discordapp.com/avatars/%s/%s.png?size=128",
             dcId, dcAvatarHash);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("User-Agent", "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) { http.end(); return; }

    int len = http.getSize();

    if (len <= 0 || len > 65536) { http.end(); return; }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { http.end(); return; }

    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < (size_t)len && millis() - t0 < 8000) {
        if (stream->available()) {
            int chunk = stream->read(buf + got, len - got);
            if (chunk > 0) got += chunk;
        } else {
            delay(1);
        }
    }
    http.end();

    if (got == (size_t)len) {
        avatarPngBuf  = buf;
        avatarPngSize = got;

        if (got >= 24) {
            avatarPngW = (int)((buf[16] << 24) | (buf[17] << 16) |
                                (buf[18] <<  8) |  buf[19]);
            avatarPngH = (int)((buf[20] << 24) | (buf[21] << 16) |
                                (buf[22] <<  8) |  buf[23]);
            Serial.printf("[Discord] Avatar PNG size from IHDR: %d x %d\n", avatarPngW, avatarPngH);
        } else {
            avatarPngW = 128;
            avatarPngH = 128;
        }
        avatarReady = true;
    } else {
        heap_caps_free(buf);
    }
}

static Rect logoutBtnRect;

static void draw_info_row(int x, int y, int w, const char* label, const char* value) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, w, 68, 8, DC_SURFACE);
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(DC_MUTED, DC_SURFACE);
    d.drawString(label, x + 16, y + 10);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_SURFACE);
    d.drawString(value, x + 16, y + 32);
}

static void draw_profile() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    draw_titlebar("Account");

    int contentW = (W > 900) ? 800 : W - 48;
    int cx       = (W - contentW) / 2;
    int contentTop = TITLEBAR_H + 24;

    int avatarR = DC_AVATAR_DIAM / 2;
    int avatarCX = cx + avatarR;
    int avatarCY = contentTop + avatarR;
    draw_avatar(avatarCX, avatarCY, avatarR);

    int nameX = avatarCX + avatarR + 28;
    int nameY = contentTop + 14;

    const char* displayName = (dcGlobalName[0] != '\0') ? dcGlobalName : dcUsername;
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_DARK_BG);
    d.drawString(displayName, nameX, nameY);

    char tag[140];
    if (strcmp(dcDiscriminator, "0") == 0 || dcDiscriminator[0] == '\0') {
        snprintf(tag, sizeof(tag), "@%s", dcUsername);
    } else {
        snprintf(tag, sizeof(tag), "%s#%s", dcUsername, dcDiscriminator);
    }
    d.setFont(&fonts::Font2);
    d.setTextColor(DC_MUTED, DC_DARK_BG);
    d.drawString(tag, nameX, nameY + 40);

    d.fillCircle(nameX + 12, nameY + 82, 10, DC_GREEN);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font2);
    d.setTextColor(DC_GREEN, DC_DARK_BG);
    d.drawString("Logged in", nameX + 28, nameY + 82);

    int rowY   = contentTop + avatarR * 2 + 32;
    int rowW   = contentW;
    int rowGap = 14;
    draw_info_row(cx, rowY,                     rowW, "USER ID",  dcId);
    draw_info_row(cx, rowY + 68 + rowGap,       rowW, "EMAIL",    dcEmail[0] ? dcEmail : "(not returned)");
    draw_info_row(cx, rowY + (68 + rowGap) * 2, rowW, "USERNAME", tag);

    int btnW = 220, btnH = 60;
    int btnX = cx;
    int btnY = H - navbar_height() - btnH - 24;
    d.fillRoundRect(btnX, btnY, btnW, btnH, 8, DC_RED);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, DC_RED);
    d.drawString("Log Out", btnX + btnW / 2, btnY + btnH / 2);
    logoutBtnRect = { btnX, btnY, btnW, btnH };

    navbar_draw("Discord");
}

static void update_profile() {
    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return;
    uint32_t now = millis();
    if (now - lastTouchMs < TOUCH_DEBOUNCE) return;
    lastTouchMs = now;

    if (navbar_touch(t.x, t.y)) return;

    if (in_rect(t.x, t.y, logoutBtnRect)) {

        nvs_clear();
        dc_gateway_stop();
        memset(authToken,      0, sizeof(authToken));
        memset(dcUsername,     0, sizeof(dcUsername));
        memset(dcGlobalName,   0, sizeof(dcGlobalName));
        memset(dcEmail,        0, sizeof(dcEmail));
        memset(dcId,           0, sizeof(dcId));
        memset(dcAvatarHash,   0, sizeof(dcAvatarHash));
        memset(passwordBuf,    0, sizeof(passwordBuf));
        avatarReady = false;
        avatarPngW  = 0;
        avatarPngH  = 0;
        if (avatarPngBuf) {
            heap_caps_free(avatarPngBuf);
            avatarPngBuf  = nullptr;
            avatarPngSize = 0;
        }
        activeField = ActiveField::NONE;
        dcScreen    = DcScreen::LOGIN;
        os.dirty    = true;
    }
}

static void draw_error() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    draw_titlebar("Error");

    int cx = W / 2;
    int cy = (H - navbar_height()) / 2;

    d.fillCircle(cx, cy - 80, 50, DC_RED);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8);
    d.setTextColor(COL_WHITE, DC_RED);
    d.drawString("!", cx, cy - 80);

    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_DARK_BG);
    char msgCopy[256];
    snprintf(msgCopy, sizeof(msgCopy), "%s", dcErrorMsg);
    int lineY = cy;
    char* line = strtok(msgCopy, "\n");
    while (line) {
        d.setTextDatum(top_center);
        d.drawString(line, cx, lineY);
        lineY += 36;
        line = strtok(nullptr, "\n");
    }

    int btnW = 300, btnH = 64;
    int btnX = cx - btnW / 2;
    int btnY = H - navbar_height() - btnH - 40;
    d.fillRoundRect(btnX, btnY, btnW, btnH, 8, DC_BLURPLE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, DC_BLURPLE);
    d.drawString("Try Again", cx, btnY + btnH / 2);
    loginBtnRect = { btnX, btnY, btnW, btnH };

    navbar_draw("Discord");
}

static void update_error() {
    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return;
    uint32_t now = millis();
    if (now - lastTouchMs < TOUCH_DEBOUNCE) return;
    lastTouchMs = now;

    if (navbar_touch(t.x, t.y)) return;

    if (in_rect(t.x, t.y, loginBtnRect)) {
        memset(passwordBuf, 0, sizeof(passwordBuf));
        memset(authToken,   0, sizeof(authToken));
        activeField = ActiveField::NONE;
        dcAutoLogin = false;
        dcScreen    = DcScreen::LOGIN;
        os.dirty    = true;
    }
}

#define DM_AVATAR_SIZE   40
#define DM_ROW_H         72
#define DM_AVATAR_RADIUS (DM_AVATAR_SIZE / 2)

struct DmAvatarCache {
    char     userId[DC_ID_LEN];
    uint16_t pixels[DM_AVATAR_SIZE * DM_AVATAR_SIZE];

    int8_t   rowX0[DM_AVATAR_SIZE];
    int8_t   rowX1[DM_AVATAR_SIZE];
    bool     loaded;
};

static DmAvatarCache* s_avatarCache = nullptr;

static int  s_dmScroll      = 0;
static int  s_dmTouchStartY = -1;
static int  s_dmScrollStart = 0;

static int  s_dmTappedRow   = -1;

static void dm_cache_init() {
    if (s_avatarCache) return;
    s_avatarCache = (DmAvatarCache*)heap_caps_calloc(
        DC_MAX_DMS, sizeof(DmAvatarCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void dm_fetch_avatar(int idx, const char* userId, const char* avatarHash) {
    if (!s_avatarCache) return;
    DmAvatarCache* slot = &s_avatarCache[idx];
    if (slot->loaded) return;

    snprintf(slot->userId, DC_ID_LEN, "%s", userId);

    for (int row = 0; row < DM_AVATAR_SIZE; row++) {
        slot->rowX0[row] = -1;
        slot->rowX1[row] = -1;
    }

    if (avatarHash[0] == '\0') {
        slot->loaded = true;
        return;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
             userId, avatarHash);

    static WiFiClientSecure s_avatarClient;
    static HTTPClient       s_avatarHttp;
    static bool             s_avatarClientInit = false;
    if (!s_avatarClientInit) {
        s_avatarClient.setInsecure();
        s_avatarClientInit = true;
    }

    dc_tls_lock();
    s_avatarHttp.setReuse(true);
    s_avatarHttp.begin(s_avatarClient, url);
    s_avatarHttp.addHeader("User-Agent", "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
    s_avatarHttp.setTimeout(8000);
    int code = s_avatarHttp.GET();
    if (code != 200) { s_avatarHttp.end(); dc_tls_unlock(); slot->loaded = true; return; }

    int len = s_avatarHttp.getSize();
    if (len <= 0 || len > 32768) { s_avatarHttp.end(); dc_tls_unlock(); slot->loaded = true; return; }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { s_avatarHttp.end(); dc_tls_unlock(); slot->loaded = true; return; }

    WiFiClient* stream = s_avatarHttp.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < (size_t)len && millis() - t0 < 6000) {
        if (stream->available()) { int n = stream->read(buf + got, len - got); if (n > 0) got += n; }
        else delay(1);
    }
    s_avatarHttp.end();
    dc_tls_unlock();

    if (got != (size_t)len) { heap_caps_free(buf); slot->loaded = true; return; }

    // avatars are downloaded and rasterized once into a small fixed-size cache,
    // pre-masked into a circle so drawing them each frame is just a memcpy
    int pngW = 64, pngH = 64;
    if (got >= 24) {
        // width/height live at fixed offsets in the PNG IHDR chunk
        pngW = (int)((buf[16]<<24)|(buf[17]<<16)|(buf[18]<<8)|buf[19]);
        pngH = (int)((buf[20]<<24)|(buf[21]<<16)|(buf[22]<<8)|buf[23]);
    }

    LGFX_Sprite tmp;
    tmp.setPsram(true);
    tmp.setColorDepth(24);

    if (tmp.createSprite(pngW, pngH)) {

        tmp.setSwapBytes(M5.Display.getSwapBytes());
        tmp.drawPng(buf, got, 0, 0);
        heap_caps_free(buf);

        int radius  = DM_AVATAR_RADIUS;
        int diam    = DM_AVATAR_SIZE;
        int sampleR = radius - 1;
        int r2      = sampleR * sampleR;

        float scaleX = (float)pngW / (float)diam;
        float scaleY = (float)pngH / (float)diam;

        uint16_t px;
        // per row, compute the circle's left/right edge and only sample/store those pixels
        for (int row = 0; row < diam; row++) {
            int dy  = row - radius;
            int dx2 = r2 - dy*dy;
            if (dx2 < 0) { slot->rowX0[row] = -1; slot->rowX1[row] = -1; continue; }
            int dx  = (int)sqrtf((float)dx2);
            int x0  = radius - dx, x1 = radius + dx;
            if (x0 < 0)         x0 = 0;
            if (x1 > diam - 1)  x1 = diam - 1;
            if (x1 < x0) { slot->rowX0[row] = -1; slot->rowX1[row] = -1; continue; }
            slot->rowX0[row] = (int8_t)x0;
            slot->rowX1[row] = (int8_t)x1;

            int srcY = (int)(row * scaleY);
            if (srcY < 0) srcY = 0;
            if (srcY >= pngH) srcY = pngH - 1;

            for (int col = x0; col <= x1; col++) {
                int srcX = (int)(col * scaleX);
                if (srcX < 0) srcX = 0;
                if (srcX >= pngW) srcX = pngW - 1;

                tmp.readRect(srcX, srcY, 1, 1, &px);
                slot->pixels[row * diam + col] = px;
            }
        }
        tmp.deleteSprite();
    } else {
        heap_caps_free(buf);
        tmp.deleteSprite();
        for (int row = 0; row < DM_AVATAR_SIZE; row++) {
            slot->rowX0[row] = -1;
            slot->rowX1[row] = -1;
        }
    }

    slot->loaded = true;
}

static void draw_dm_avatar(int screenX, int screenY, int idx,
                            const char* username, const char* avatarHash) {
    auto& d = M5.Display;
    int diam   = DM_AVATAR_SIZE;
    int radius = DM_AVATAR_RADIUS;
    int cx     = screenX + radius;
    int cy     = screenY + radius;

    bool hasPixels = s_avatarCache && s_avatarCache[idx].loaded &&
                     avatarHash[0] != '\0';

    if (hasPixels) {

        d.fillCircle(cx, cy, radius, DC_DARK_BG);
        DmAvatarCache& cache = s_avatarCache[idx];
        for (int row = 0; row < diam; row++) {
            int x0 = cache.rowX0[row];
            int x1 = cache.rowX1[row];
            if (x0 < 0 || x1 < x0) continue;
            d.pushImage(screenX + x0, screenY + row, x1 - x0 + 1, 1,
                        &cache.pixels[row * diam + x0]);
        }
    } else {

        d.fillCircle(cx, cy, radius, DC_BLURPLE);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_WHITE, DC_BLURPLE);
        char ini[2] = { username[0] ? (char)toupper((unsigned char)username[0]) : '?', '\0' };
        d.drawString(ini, cx, cy);
    }
}

static void draw_status_dot(int cx, int cy, DcStatus status, uint32_t bgColour) {
    auto& d = M5.Display;
    const int R  = 7;
    const int RB = 9;

    d.fillCircle(cx, cy, RB, bgColour);

    uint32_t col = dc_status_colour(status);
    d.fillCircle(cx, cy, R, col);

    if (status == DcStatus::IDLE) {

        d.fillCircle(cx + 3, cy - 3, R - 1, bgColour);
    }

    if (status == DcStatus::DND) {

        d.fillRect(cx - R + 3, cy - 2, (R - 3) * 2, 4, 0xFFFFFF);
    }
}

static const char* activity_prefix(DcActivityType t) {
    switch (t) {
        case DcActivityType::LISTENING: return "\xE2\x99\xAB ";
        case DcActivityType::PLAYING:   return "";
        case DcActivityType::CUSTOM:    return "";
        default:                        return "";
    }
}

static void draw_gw_status_bar() {
    auto& d = M5.Display;
    int W = d.width();

    const char* label;
    uint32_t col;
    switch (dc_gw_state) {
        case DcGwState::CONNECTING:
        case DcGwState::IDENTIFYING:
            label = "Connecting..."; col = DC_MUTED; break;
        case DcGwState::READY:
            return;
        case DcGwState::ERROR:
            label = "Gateway error — retrying"; col = DC_RED; break;
        default:
            label = "Disconnected"; col = DC_MUTED; break;
    }

    d.fillRect(0, TITLEBAR_H, W, 28, DC_DARKER_BG);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(col, DC_DARKER_BG);
    d.drawString(label, W / 2, TITLEBAR_H + 14);
}

static void draw_dm_list() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(DC_DARK_BG);
    draw_titlebar("Direct Messages");
    draw_gw_status_bar();

    int gwBarH = (dc_gw_state == DcGwState::READY) ? 0 : 28;
    int listTop = TITLEBAR_H + gwBarH;
    int listH   = H - listTop - navbar_height();

    dc_gw_lock();
    int count = dc_dm_count;

    int visibleRows = listH / DM_ROW_H;

    if (s_dmScroll > count - visibleRows) s_dmScroll = max(0, count - visibleRows);
    if (s_dmScroll < 0) s_dmScroll = 0;

    int y = listTop;
    for (int i = s_dmScroll; i < count && y < listTop + listH; i++, y += DM_ROW_H) {
        DcDmEntry& dm = dc_dms[i];

        bool hover = false;
        d.fillRect(0, y, W, DM_ROW_H - 1, hover ? DC_SURFACE : DC_DARK_BG);

        d.drawFastHLine(0, y + DM_ROW_H - 1, W, DC_SURFACE);

        int avY  = y + (DM_ROW_H - DM_AVATAR_SIZE) / 2;
        int avX  = 16;
        draw_dm_avatar(avX, avY, i, dm.username, dm.avatarHash);

        int dotX = avX + DM_AVATAR_SIZE - 2;
        int dotY = avY + DM_AVATAR_SIZE - 2;
        draw_status_dot(dotX, dotY, dm.status, DC_DARK_BG);

        int textX = avX + DM_AVATAR_SIZE + 14;
        int nameY = y + 14;
        const char* displayName = (dm.globalName[0] != '\0') ? dm.globalName : dm.username;

        d.setTextDatum(top_left);
        d.setFont(&fonts::Font4);
        d.setTextColor(DC_TEXT, DC_DARK_BG);

        char nameTrunc[DC_NAME_LEN];
        snprintf(nameTrunc, sizeof(nameTrunc), "%s", displayName);
        d.drawString(nameTrunc, textX, nameY);

        int actY = nameY + 28;
        if (dm.activityText[0] != '\0') {
            char actLine[DC_STATUS_LEN + 8];
            snprintf(actLine, sizeof(actLine), "%s%s",
                     activity_prefix(dm.activityType), dm.activityText);
            d.setFont(&fonts::Font2);
            d.setTextColor(DC_MUTED, DC_DARK_BG);
            d.drawString(actLine, textX, actY);
        } else if (dm.status == DcStatus::OFFLINE || dm.status == DcStatus::UNKNOWN) {
            d.setFont(&fonts::Font2);
            d.setTextColor(DC_MUTED, DC_DARK_BG);
            d.drawString("Offline", textX, actY);
        }
    }

    if (count == 0) {
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font4);
        d.setTextColor(DC_MUTED, DC_DARK_BG);
        d.drawString(dc_gw_state == DcGwState::READY ?
                     "No DMs yet" : "Loading DMs...",
                     W / 2, listTop + listH / 2);
    }

    dc_gw_unlock();

    navbar_draw("Discord");
}

static TaskHandle_t s_avatarTask = nullptr;

static void avatar_fetch_task(void*) {
    for (;;) {

        if (dcScreen != DcScreen::DM_LIST) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        dm_cache_init();
        dc_gw_lock();
        int count = dc_dm_count;
        int pending = -1;
        char uid[DC_ID_LEN] = "", hash[DC_AVATAR_LEN] = "";
        for (int i = 0; i < count; i++) {
            if (s_avatarCache[i].loaded &&
                strcmp(s_avatarCache[i].userId, dc_dms[i].userId) != 0) {
                s_avatarCache[i].loaded = false;
            }
            if (!s_avatarCache[i].loaded) {
                pending = i;
                snprintf(uid,  DC_ID_LEN,     "%s", dc_dms[i].userId);
                snprintf(hash, DC_AVATAR_LEN, "%s", dc_dms[i].avatarHash);
                break;
            }
        }
        dc_gw_unlock();

        if (pending >= 0) {
            dm_fetch_avatar(pending, uid, hash);
            os.dirty = true;
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void avatar_fetch_task_start() {
    if (s_avatarTask) return;
    xTaskCreatePinnedToCore(avatar_fetch_task, "dc_avatars", 8192, nullptr, 1, &s_avatarTask, 0);
}

static void update_dm_list() {

    static uint32_t lastRedraw = 0;
    uint32_t now = millis();
    if (now - lastRedraw > 2000) {
        lastRedraw = now;
        os.dirty   = true;
    }

    avatar_fetch_task_start();

    auto t = M5.Touch.getDetail();

    if (t.wasPressed()) {
        s_dmTouchStartY = t.y;
        s_dmScrollStart = s_dmScroll;
        s_dmTappedRow   = -1;
    }

    if (t.isPressed() && s_dmTouchStartY >= 0) {
        int delta = (s_dmTouchStartY - t.y) / DM_ROW_H;
        int newScroll = s_dmScrollStart + delta;
        if (newScroll != s_dmScroll) {
            s_dmScroll = newScroll;
            os.dirty   = true;
        }
    }

    if (t.wasClicked()) {
        uint32_t ts = millis();
        if (ts - lastTouchMs < TOUCH_DEBOUNCE) return;
        lastTouchMs = ts;
        if (navbar_touch(t.x, t.y)) return;

        int gwBarH  = (dc_gw_state == DcGwState::READY) ? 0 : 28;
        int listTop = TITLEBAR_H + gwBarH;
        if (t.y >= listTop) {
            int row = s_dmScroll + (t.y - listTop) / DM_ROW_H;
            dc_gw_lock();
            int count = dc_dm_count;
            dc_gw_unlock();
            if (row >= 0 && row < count) {
                open_dm_channel(row);
            }
        }
    }
}
#define DC_MAX_MSGS   20
#define DC_MSG_LEN    200
#define DC_TS_LEN     40

struct DcMessage {
    char    id[DC_ID_LEN];
    char    authorId[DC_ID_LEN];
    char    authorName[DC_NAME_LEN];
    char    content[DC_MSG_LEN];
    char    isoTimestamp[DC_TS_LEN];
    char    displayTime[48];
};

static SemaphoreHandle_t s_msgMutex   = nullptr;
static DcMessage         s_messages[DC_MAX_MSGS];
static int               s_messageCount = 0;
static bool              s_msgLoading   = false;
static bool              s_msgLoadFailed = false;

static char s_curChannelId[DC_ID_LEN]    = "";
static char s_curUserId[DC_ID_LEN]       = "";
static char s_curUsername[DC_NAME_LEN]   = "";
static char s_curGlobalName[DC_NAME_LEN] = "";
static char s_curAvatarHash[DC_AVATAR_LEN] = "";
static DcStatus s_curStatus = DcStatus::UNKNOWN;

static int find_dm_index_by_userid(const char* userId) {
    if (!userId || userId[0] == '\0') return -1;
    dc_gw_lock();
    int found = -1;
    for (int i = 0; i < dc_dm_count; i++) {
        if (strcmp(dc_dms[i].userId, userId) == 0) { found = i; break; }
    }
    dc_gw_unlock();
    return found;
}

static int  s_msgScroll        = 0;
static int  s_msgTouchStartY   = -1;
static int  s_msgScrollStart   = 0;
static char s_msgInputBuf[DC_MSG_LEN] = "";
static Rect s_msgInputRect;
static Rect s_msgSendRect;
static Rect s_msgBackRect;
static bool s_msgSending       = false;

static void msg_mutex_init() {
    if (!s_msgMutex) s_msgMutex = xSemaphoreCreateMutex();
}
static void msg_lock()   { msg_mutex_init(); xSemaphoreTake(s_msgMutex, portMAX_DELAY); }
static void msg_unlock() { xSemaphoreGive(s_msgMutex); }

static const char* kMonthAbbr[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

static bool parse_iso_timestamp(const char* iso, int* y, int* mo, int* d,
                                 int* h, int* mi) {
    int sec = 0;
    int n = sscanf(iso, "%d-%d-%dT%d:%d:%d", y, mo, d, h, mi, &sec);
    return n >= 5;
}

// renders a message timestamp the way Discord's own client does: bare time for
// today, "Yesterday HH:MM", otherwise a full date — computed by hand since we
// don't have a real calendar library available here
static void format_msg_datetime(const char* iso, char* out, size_t outSize) {
    int msgY, msgMo, msgD, msgH, msgMi;
    if (!parse_iso_timestamp(iso, &msgY, &msgMo, &msgD, &msgH, &msgMi)) {
        snprintf(out, outSize, "%s", iso);
        return;
    }

    time_t now = time(nullptr);
    struct tm nowTm;
    gmtime_r(&now, &nowTm);
    int curY  = nowTm.tm_year + 1900;
    int curMo = nowTm.tm_mon + 1;
    int curD  = nowTm.tm_mday;

    bool haveClock = (now > 1700000000);

    if (haveClock && msgY == curY && msgMo == curMo && msgD == curD) {

        snprintf(out, outSize, "%02d:%02d", msgH, msgMi);
        return;
    }

    if (haveClock) {

        static const int kDaysInMonth[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        int yY = curY, yMo = curMo, yD = curD - 1;
        if (yD < 1) {
            yMo -= 1;
            if (yMo < 1) { yMo = 12; yY -= 1; }
            int dim = kDaysInMonth[yMo - 1];

            if (yMo == 2 && ((yY % 4 == 0 && yY % 100 != 0) || yY % 400 == 0)) dim = 29;
            yD = dim;
        }
        if (msgY == yY && msgMo == yMo && msgD == yD) {
            snprintf(out, outSize, "Yesterday %02d:%02d", msgH, msgMi);
            return;
        }
    }

    const char* mon = (msgMo >= 1 && msgMo <= 12) ? kMonthAbbr[msgMo - 1] : "???";
    snprintf(out, outSize, "%d %s %d %02d:%02d", msgD, mon, msgY, msgH, msgMi);
}

static TaskHandle_t s_msgFetchTask = nullptr;

static char s_msgLastError[96] = "";

static void msg_fetch_task(void* arg) {
    char channelId[DC_ID_LEN];
    snprintf(channelId, sizeof(channelId), "%s", (const char*)arg);
    free(arg);

    os.dirty = true;
    s_msgLastError[0] = '\0';

    bool ok = false;
    do {
        if (WiFi.status() != WL_CONNECTED) {
            snprintf(s_msgLastError, sizeof(s_msgLastError), "No Wi-Fi");
            Serial.println("[Discord] msg fetch: no Wi-Fi");
            break;
        }

        char url[192];
        snprintf(url, sizeof(url),
                 "https://discord.com/api/v9/channels/%s/messages?limit=%d",
                 channelId, DC_MAX_MSGS);

        // drop the gateway connection while we do this plain HTTP fetch, then
        // bring it back up — see the TLS-sharing note in discord_gateway.h
        dc_gateway_stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        http.addHeader("Authorization", authToken);
        http.addHeader("User-Agent", "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
        http.setTimeout(12000);
        int code = http.GET();
        String payload = http.getString();
        http.end();

        dc_gateway_start(authToken);
        Serial.printf("[Discord] msg fetch: HTTP %d, %u bytes\n", code, (unsigned)payload.length());
        if (code != 200) {

            Serial.printf("[Discord] msg fetch body: %s\n", payload.c_str());
            snprintf(s_msgLastError, sizeof(s_msgLastError), "HTTP %d", code);
            break;
        }

        StaticJsonDocument<512> filter;
        JsonObject elemFilter = filter.createNestedObject((size_t)0);
        elemFilter["id"]        = true;
        elemFilter["content"]   = true;
        elemFilter["timestamp"] = true;
        JsonObject authorFilter = elemFilter.createNestedObject("author");
        authorFilter["id"]          = true;
        authorFilter["username"]    = true;
        authorFilter["global_name"] = true;

        DynamicJsonDocument doc(32768);
        auto err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
        if (err) {
            Serial.printf("[Discord] msg fetch: JSON parse failed: %s\n", err.c_str());
            snprintf(s_msgLastError, sizeof(s_msgLastError), "Parse error: %s", err.c_str());
            break;
        }
        if (doc.overflowed()) {
            Serial.println("[Discord] msg fetch: JSON doc overflowed");
            snprintf(s_msgLastError, sizeof(s_msgLastError), "JSON overflow");
            break;
        }
        if (!doc.is<JsonArray>()) {
            Serial.printf("[Discord] msg fetch: response wasn't a JSON array, first 200: %.200s\n", payload.c_str());
            snprintf(s_msgLastError, sizeof(s_msgLastError), "Unexpected response shape");
            break;
        }

        JsonArray arr = doc.as<JsonArray>();
        int n = arr.size();
        Serial.printf("[Discord] msg fetch: parsed %d messages\n", n);
        if (n > DC_MAX_MSGS) n = DC_MAX_MSGS;

        msg_lock();
        s_messageCount = n;

        for (int i = 0; i < n; i++) {
            JsonObject m = arr[n - 1 - i];
            DcMessage& out = s_messages[i];
            snprintf(out.id,           DC_ID_LEN,   "%s", (const char*)(m["id"] | ""));
            JsonObject author = m["author"];
            snprintf(out.authorId,     DC_ID_LEN,   "%s", (const char*)(author["id"] | ""));
            const char* gname = author["global_name"] | "";
            const char* uname = author["username"]    | "";
            snprintf(out.authorName, DC_NAME_LEN, "%s", gname[0] ? gname : uname);
            snprintf(out.content,      DC_MSG_LEN,  "%s", (const char*)(m["content"] | ""));
            snprintf(out.isoTimestamp, DC_TS_LEN,   "%s", (const char*)(m["timestamp"] | ""));
            format_msg_datetime(out.isoTimestamp, out.displayTime, sizeof(out.displayTime));
        }
        msg_unlock();
        ok = true;
    } while (0);

    s_msgLoading    = false;
    s_msgLoadFailed = !ok;
    os.dirty        = true;
    s_msgFetchTask  = nullptr;
    vTaskDelete(nullptr);
}

static void start_msg_fetch(const char* channelId) {
    if (s_msgFetchTask) return;
    s_msgLoading    = true;
    s_msgLoadFailed = false;
    char* arg = (char*)malloc(DC_ID_LEN);
    snprintf(arg, DC_ID_LEN, "%s", channelId);
    xTaskCreatePinnedToCore(msg_fetch_task, "dc_msgfetch", 12288, arg, 1, &s_msgFetchTask, 0);
}

struct DcSendArgs {
    char channelId[DC_ID_LEN];
    char content[DC_MSG_LEN];
};

static void json_escape_into(const char* in, char* out, size_t outCap) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < outCap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')        {  }
        else if (c < 0x20)         {  }
        else                       { out[o++] = c; }
    }
    out[o] = '\0';
}

static void msg_send_task(void* arg) {
    DcSendArgs* a = (DcSendArgs*)arg;

    if (WiFi.status() == WL_CONNECTED) {
        char escaped[DC_MSG_LEN * 2];
        json_escape_into(a->content, escaped, sizeof(escaped));

        char body[DC_MSG_LEN * 2 + 64];
        snprintf(body, sizeof(body), "{\"content\":\"%s\",\"tts\":false}", escaped);

        char url[192];
        snprintf(url, sizeof(url), "https://discord.com/api/v9/channels/%s/messages", a->channelId);

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", authToken);
        http.addHeader("User-Agent", "P4TabOS/1.0 (M5Stack Tab5; ESP32-P4)");
        http.setTimeout(12000);
        http.POST(body);
        http.end();
    }

    if (strcmp(a->channelId, s_curChannelId) == 0) {
        start_msg_fetch(a->channelId);
    }

    delete a;
    s_msgSending = false;
    os.dirty     = true;
    vTaskDelete(nullptr);
}

static void send_current_message() {
    if (s_msgInputBuf[0] == '\0' || s_curChannelId[0] == '\0') return;
    if (s_msgSending) return;

    DcSendArgs* a = new DcSendArgs();
    snprintf(a->channelId, sizeof(a->channelId), "%s", s_curChannelId);
    snprintf(a->content,   sizeof(a->content),   "%s", s_msgInputBuf);

    s_msgSending = true;
    s_msgInputBuf[0] = '\0';

    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(msg_send_task, "dc_msgsend", 4096, a, 1, &h, 0);
}

static void open_dm_channel(int dmIndex) {
    dc_gw_lock();
    if (dmIndex < 0 || dmIndex >= dc_dm_count) { dc_gw_unlock(); return; }
    DcDmEntry& dm = dc_dms[dmIndex];
    snprintf(s_curChannelId,   sizeof(s_curChannelId),   "%s", dm.channelId);
    snprintf(s_curUserId,      sizeof(s_curUserId),      "%s", dm.userId);
    snprintf(s_curUsername,    sizeof(s_curUsername),    "%s", dm.username);
    snprintf(s_curGlobalName,  sizeof(s_curGlobalName),  "%s", dm.globalName);
    snprintf(s_curAvatarHash,  sizeof(s_curAvatarHash),  "%s", dm.avatarHash);
    s_curStatus = dm.status;
    dc_gw_unlock();

    msg_lock();
    s_messageCount = 0;
    msg_unlock();

    s_msgScroll      = 0;
    s_msgInputBuf[0] = '\0';
    s_msgSending     = false;

    dcScreen = DcScreen::DM_CHANNEL;
    os.dirty = true;

    start_msg_fetch(s_curChannelId);
}

static const int DM_CHAN_HEADER_H = 64;
static const int DM_INPUT_H       = 64;
static const int DM_INPUT_MARGIN  = 12;

static void draw_dm_channel_header() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillRect(0, 0, W, DM_CHAN_HEADER_H, DC_DARKER_BG);
    d.drawFastHLine(0, DM_CHAN_HEADER_H, W, DC_SURFACE);

    s_msgBackRect = { 8, 0, 56, DM_CHAN_HEADER_H };
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_DARKER_BG);
    d.drawString("<", s_msgBackRect.x + s_msgBackRect.w / 2, DM_CHAN_HEADER_H / 2);

    int avR  = DM_AVATAR_RADIUS;
    int avCX = 76 + avR;
    int avCY = DM_CHAN_HEADER_H / 2;
    int idx  = find_dm_index_by_userid(s_curUserId);
    if (idx >= 0) {
        draw_dm_avatar(avCX - avR, avCY - avR, idx, s_curUsername, s_curAvatarHash);
    } else {

        d.fillCircle(avCX, avCY, avR, DC_BLURPLE);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_WHITE, DC_BLURPLE);
        char ini[2] = { s_curUsername[0] ? (char)toupper((unsigned char)s_curUsername[0]) : '?', '\0' };
        d.drawString(ini, avCX, avCY);
    }
    draw_status_dot(avCX + avR - 2, avCY + avR - 2, s_curStatus, DC_DARKER_BG);

    const char* displayName = s_curGlobalName[0] ? s_curGlobalName : s_curUsername;
    d.setTextDatum(middle_left);
    d.setFont(&fonts::Font4);
    d.setTextColor(DC_TEXT, DC_DARKER_BG);
    d.drawString(displayName, avCX + avR + 16, DM_CHAN_HEADER_H / 2);
}

static int wrap_text(const char* text, int maxCharsPerLine, char lineOut[][DC_MSG_LEN], int maxLines) {
    int lineCount = 0;
    char buf[DC_MSG_LEN];
    snprintf(buf, sizeof(buf), "%s", text);

    char* word = strtok(buf, " ");
    char cur[DC_MSG_LEN] = "";
    while (word && lineCount < maxLines) {
        size_t curLen  = strlen(cur);
        size_t wordLen = strlen(word);
        size_t needed  = curLen + (curLen ? 1 : 0) + wordLen;
        if ((int)needed > maxCharsPerLine && curLen > 0) {
            snprintf(lineOut[lineCount++], DC_MSG_LEN, "%s", cur);
            cur[0] = '\0';
            curLen = 0;
        }
        if (curLen) strncat(cur, " ", sizeof(cur) - strlen(cur) - 1);
        strncat(cur, word, sizeof(cur) - strlen(cur) - 1);
        word = strtok(nullptr, " ");
    }
    if (cur[0] && lineCount < maxLines) snprintf(lineOut[lineCount++], DC_MSG_LEN, "%s", cur);
    return lineCount;
}

static void draw_dm_channel() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(DC_DARK_BG);
    draw_dm_channel_header();

    int inputH  = keyboard_is_open() ? 0 : DM_INPUT_H + DM_INPUT_MARGIN * 2;
    int bottomH = keyboard_is_open() ? keyboard_height() : (inputH + navbar_height());
    int listTop = DM_CHAN_HEADER_H;
    int listH   = H - listTop - bottomH;

    msg_lock();
    int count = s_messageCount;

    const int avR       = DM_AVATAR_RADIUS;
    const int avDiam    = avR * 2;
    const int textX     = 16 + avDiam + 12;
    const int wrapChars = max(10, (W - textX - 20) / 7);
    const int lineH     = 24;
    const int groupGapY = 10;
    const int partnerIdx = find_dm_index_by_userid(s_curUserId);

    int totalH = 0;
    static char wrapBuf[24][DC_MSG_LEN];
    static int  wrapLines[DC_MAX_MSGS];
    static bool newGroup[DC_MAX_MSGS];
    for (int i = 0; i < count; i++) {
        newGroup[i] = (i == 0) || strcmp(s_messages[i].authorId, s_messages[i - 1].authorId) != 0;
        int maxLines = 24;
        int nl = wrap_text(s_messages[i].content, wrapChars, wrapBuf, maxLines);
        if (nl == 0) nl = 1;
        wrapLines[i] = nl;
        int h = nl * lineH + (newGroup[i] ? (groupGapY + 22) : 4);
        totalH += h;
    }

    int startY = listTop + listH - totalH + s_msgScroll;
    if (startY > listTop) startY = listTop;

    int maxScroll = max(0, totalH - listH);
    if (s_msgScroll > maxScroll) s_msgScroll = maxScroll;
    if (s_msgScroll < 0)         s_msgScroll = 0;

    d.setClipRect(0, listTop, W, listH);
    d.fillRect(0, listTop, W, listH, DC_DARK_BG);

    int y = startY;
    for (int i = 0; i < count; i++) {
        bool isMe = (strcmp(s_messages[i].authorId, dcId) == 0);

        if (newGroup[i]) {
            y += groupGapY;
            int avCY = y + avR - 5;
            if (isMe) {
                draw_avatar(16 + avR, avCY, avR);
            } else if (partnerIdx >= 0) {
                draw_dm_avatar(16, avCY - avR, partnerIdx, s_messages[i].authorName,
                               s_curAvatarHash);
            } else {
                M5.Display.fillCircle(16 + avR, avCY, avR, DC_BLURPLE);
                M5.Display.setTextDatum(middle_center);
                M5.Display.setFont(&fonts::Font2);
                M5.Display.setTextColor(COL_WHITE, DC_BLURPLE);
                char ini[2] = { s_messages[i].authorName[0]
                                ? (char)toupper((unsigned char)s_messages[i].authorName[0]) : '?', '\0' };
                M5.Display.drawString(ini, 16 + avR, avCY);
            }

            d.setTextDatum(top_left);
            d.setFont(&fonts::Font2);
            d.setTextColor(DC_TEXT, DC_DARK_BG);
            d.drawString(s_messages[i].authorName, textX, y);

            int nameW = (int)strnlen(s_messages[i].authorName, DC_NAME_LEN) * 8 + 10;
            d.setTextColor(DC_MUTED, DC_DARK_BG);
            d.drawString(s_messages[i].displayTime, textX + nameW, y);

            y += 22;
        }

        d.setTextDatum(top_left);
        d.setFont(&fonts::Font2);
        d.setTextColor(DC_TEXT, DC_DARK_BG);
        int nl = wrapLines[i];

        static char lines[24][DC_MSG_LEN];
        int  actualNl = wrap_text(s_messages[i].content, wrapChars, lines, 24);
        if (actualNl == 0) actualNl = 1;
        for (int ln = 0; ln < actualNl; ln++) {
            d.drawString(lines[ln], textX, y + ln * lineH);
        }
        y += nl * lineH + 4;
    }

    if (count == 0) {
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font4);
        d.setTextColor(DC_MUTED, DC_DARK_BG);
        if (s_msgLoading) {
            d.drawString("Loading messages...", W / 2, listTop + listH / 2);
        } else if (s_msgLoadFailed) {
            d.drawString("Couldn't load messages", W / 2, listTop + listH / 2 - 16);
            if (s_msgLastError[0]) {
                d.setFont(&fonts::Font2);
                d.drawString(s_msgLastError, W / 2, listTop + listH / 2 + 16);
            }
        } else {
            d.drawString("No messages yet", W / 2, listTop + listH / 2);
        }
    }
    d.clearClipRect();
    msg_unlock();

    if (keyboard_is_open()) {
        keyboard_draw();
    } else {
        int barY = H - navbar_height() - inputH + DM_INPUT_MARGIN;
        s_msgInputRect = { 16, barY, W - 16 - 96 - 12, DM_INPUT_H };
        d.fillRoundRect(s_msgInputRect.x, s_msgInputRect.y, s_msgInputRect.w, s_msgInputRect.h,
                         DM_INPUT_H / 2, DC_SURFACE);
        d.setTextDatum(middle_left);
        d.setFont(&fonts::Font4);
        if (s_msgInputBuf[0]) {
            d.setTextColor(DC_TEXT, DC_SURFACE);
            d.drawString(s_msgInputBuf, s_msgInputRect.x + 20, s_msgInputRect.y + s_msgInputRect.h / 2);
        } else {
            d.setTextColor(DC_MUTED, DC_SURFACE);
            char placeholder[96];
            const char* nm = s_curGlobalName[0] ? s_curGlobalName : s_curUsername;
            snprintf(placeholder, sizeof(placeholder), "Message @%s", nm);
            d.drawString(placeholder, s_msgInputRect.x + 20, s_msgInputRect.y + s_msgInputRect.h / 2);
        }

        s_msgSendRect = { s_msgInputRect.x + s_msgInputRect.w + 12, barY, 80, DM_INPUT_H };
        uint32_t sendCol = s_msgSending ? DC_MUTED : DC_BLURPLE;
        d.fillRoundRect(s_msgSendRect.x, s_msgSendRect.y, s_msgSendRect.w, s_msgSendRect.h, 8, sendCol);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_WHITE, sendCol);
        d.drawString(s_msgSending ? "..." : "Send",
                     s_msgSendRect.x + s_msgSendRect.w / 2, s_msgSendRect.y + s_msgSendRect.h / 2);

        navbar_draw("Discord");
    }
}

static void update_dm_channel() {

    static uint32_t lastRedraw = 0;
    uint32_t now = millis();
    if ((s_msgLoading || s_msgSending) && now - lastRedraw > 300) {
        lastRedraw = now;
        os.dirty = true;
    }

    auto t = M5.Touch.getDetail();

    if (keyboard_is_open()) {
        if (!t.wasClicked()) return;
        auto kr = keyboard_touch(t.x, t.y);
        if (kr.handled) {
            os.dirty = true;
            if (kr.submitted) {
                keyboard_close();
                send_current_message();
                os.dirty = true;
            } else if (kr.cancelled) {
                keyboard_close();
                os.dirty = true;
            }
        }
        return;
    }

    if (t.wasPressed()) {
        s_msgTouchStartY = t.y;
        s_msgScrollStart = s_msgScroll;
    }
    if (t.isPressed() && s_msgTouchStartY >= 0) {
        int delta = t.y - s_msgTouchStartY;
        int newScroll = s_msgScrollStart - delta;
        if (newScroll != s_msgScroll) {
            s_msgScroll = newScroll < 0 ? 0 : newScroll;
            os.dirty = true;
        }
    }
    if (t.wasReleased()) {
        s_msgTouchStartY = -1;
    }

    if (t.wasClicked()) {
        uint32_t ts = millis();
        if (ts - lastTouchMs < TOUCH_DEBOUNCE) return;
        lastTouchMs = ts;

        if (in_rect(t.x, t.y, s_msgBackRect)) {
            dcScreen = DcScreen::DM_LIST;
            os.dirty = true;
            return;
        }
        if (navbar_touch(t.x, t.y)) return;

        if (in_rect(t.x, t.y, s_msgInputRect)) {
            keyboard_open(s_msgInputBuf, sizeof(s_msgInputBuf), false, "Send",
                          "Message");
            os.dirty = true;
            return;
        }
        if (in_rect(t.x, t.y, s_msgSendRect)) {
            send_current_message();
            os.dirty = true;
            return;
        }
    }
}
