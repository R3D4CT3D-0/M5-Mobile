#pragma once
#include <M5Unified.h>
#ifndef GFX_DRIVER_INTERNAL
#include "gfx_driver.h"
#endif

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH  1280
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 720
#endif

enum class Screen {
    BOOT,
    HOME,
    APP_CLOCK,
    APP_SYSINFO,
    APP_SETTINGS,
    APP_NOTEPAD,
    APP_FLAPPY,
    APP_CAMERA,
    APP_DISCORD,
};

struct OSState {
    Screen current  = Screen::BOOT;
    Screen previous = Screen::BOOT;
    bool   dirty    = true;
    uint32_t bootStartMs = 0;

    uint32_t accentColor   = 0x1A73E8U;
    bool     wallpaperGrid = true;
    uint8_t  brightness    = 200;
};

extern OSState os;

void boot_draw();
void boot_update();

void home_draw();
void home_update();

void clock_app_draw();
void clock_app_update();

void sysinfo_app_draw();
void sysinfo_app_update();

void settings_app_draw();
void settings_app_update();

void notepad_init();
void notepad_app_draw();
void notepad_app_update();

void flappy_app_draw();
void flappy_app_update();

void camera_app_draw();
void camera_app_update();
void probeVideoDevices();
void camera_app_stop();
bool camera_uses_exclusive_i2c();

void discord_app_init();
void discord_app_exit();
void discord_app_draw();
void discord_app_update();

void os_goto(Screen s);

void os_force_redraw();

void navbar_draw(const char* title);
bool navbar_touch(int tx, int ty);
int  navbar_height();

struct KeyboardResult {
    bool handled   = false;
    bool submitted = false;
    bool cancelled = false;
    bool changed   = false;
};

void keyboard_open(char* buffer, int capacity, bool obscure,
                    const char* doneLabel, const char* fieldLabel);
void keyboard_close();
bool keyboard_is_open();
int  keyboard_height();
void keyboard_draw();
KeyboardResult keyboard_touch(int tx, int ty);

int  battery_get_percent();
bool battery_is_charging();

void rotation_init();
void rotation_update();
void rotation_set_auto(bool enabled);
bool rotation_get_auto();

void settings_load();
void settings_save();

#define VENDOR_NAME   "WHTV"
#define OS_NAME       "P4-Tab OS"
#define OS_VERSION    "1.0"

#define COL_BLACK       0x000000U
#define COL_WHITE       0xFFFFFFU
#define COL_ACCENT      0x1A73E8U
#define COL_SURFACE     0x1E1E2EU
#define COL_CARD        0x2A2A3EU
#define COL_TEXT_DIM    0x888888U
#define COL_STATUSBAR   0x111122U
#define COL_DANGER      0xE8491DU
#define COL_OK          0x00CC44U
