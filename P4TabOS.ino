#include <M5Unified.h>
#include "p4_os.h"
#include "discord_gateway.h"
#include "bsod.h"

OSState os;

void os_goto(Screen s) {
    os.previous = os.current;
    os.current  = s;
    os.dirty    = true;

    // clean up whichever app we're leaving
    if (os.previous == Screen::APP_CAMERA && s != Screen::APP_CAMERA) {
        camera_app_stop();
    }

    if (os.previous == Screen::APP_DISCORD && s != Screen::APP_DISCORD) {
        discord_app_exit();
    }

    if (s == Screen::APP_DISCORD) {
        discord_app_init();
    }

    gfx_flush_wait();

    M5.Display.fillScreen(COL_BLACK);
    M5.Display.clearDirty();
}

void os_force_redraw() {
    switch (os.current) {
        case Screen::BOOT:         boot_draw();          break;
        case Screen::HOME:         home_draw();          break;
        case Screen::APP_CLOCK:    clock_app_draw();     break;
        case Screen::APP_SYSINFO:  sysinfo_app_draw();   break;
        case Screen::APP_SETTINGS: settings_app_draw();  break;
        case Screen::APP_NOTEPAD:   notepad_app_draw();    break;
        case Screen::APP_FLAPPY:    flappy_app_draw();     break;
        case Screen::APP_CAMERA:    camera_app_draw();     break;
        case Screen::APP_DISCORD:   discord_app_draw();    break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    bsod_install();

    Serial.println("[boot] setup() start");

    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.println("[boot] M5.begin() done");

    gfx_init();

    M5.Display.setRotation(1);

    M5.Display.setBrightness(200);
    M5.Display.fillScreen(COL_BLACK);

    rotation_init();

    settings_load();

    notepad_init();
    os.bootStartMs = millis();
    os.current = Screen::BOOT;
    os.dirty   = true;
}

void loop() {
    uint32_t frameStart = millis();

    // camera app takes the i2c bus for itself, so skip anything else that touches it this frame
    bool camSteals_i2c = (os.current == Screen::APP_CAMERA) && camera_uses_exclusive_i2c();
    if (!camSteals_i2c) {
        M5.update();
    }
    if (!camSteals_i2c) {
        rotation_update();
    }

    if (os.dirty) {
        os.dirty = false;
        switch (os.current) {
            case Screen::BOOT:        boot_draw();       break;
            case Screen::HOME:        home_draw();       break;
            case Screen::APP_CLOCK:   clock_app_draw();  break;
            case Screen::APP_SYSINFO: sysinfo_app_draw(); break;
            case Screen::APP_SETTINGS: settings_app_draw(); break;
        case Screen::APP_NOTEPAD:   notepad_app_draw();   break;
        case Screen::APP_FLAPPY:    flappy_app_draw();    break;
        case Screen::APP_CAMERA:    camera_app_draw();    break;
        case Screen::APP_DISCORD:   discord_app_draw();   break;
        }
    }

    switch (os.current) {
        case Screen::BOOT:        boot_update();        break;
        case Screen::HOME:        home_update();        break;
        case Screen::APP_CLOCK:   clock_app_update();   break;
        case Screen::APP_SYSINFO: sysinfo_app_update(); break;
        case Screen::APP_SETTINGS: settings_app_update(); break;
        case Screen::APP_NOTEPAD:   notepad_app_update();   break;
        case Screen::APP_FLAPPY:    flappy_app_update();    break;
        case Screen::APP_CAMERA:    camera_app_update();    break;
        case Screen::APP_DISCORD:   discord_app_update();   break;
    }

    gfx_flush();

    // cap to ~60fps
    uint32_t elapsed = millis() - frameStart;
    if (elapsed < 16) delay(16 - elapsed);
}

