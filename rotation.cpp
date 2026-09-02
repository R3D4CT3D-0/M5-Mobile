#include <M5Unified.h>
#include "p4_os.h"
#include <cmath>

static const uint8_t ROT_LANDSCAPE_USB_RIGHT = 1;
static const uint8_t ROT_LANDSCAPE_USB_LEFT  = 3;
static const uint8_t ROT_PORTRAIT_USB_DOWN   = 0;
static const uint8_t ROT_PORTRAIT_USB_UP     = 2;

static const float    TILT_THRESHOLD_G   = 0.35f;
static const uint32_t ROTATE_DEBOUNCE_MS = 400;
static const uint32_t SAMPLE_INTERVAL_MS = 100;

static const int      FADE_STEPS        = 12;
static const int      FADE_STEP_DELAY   = 12;

static uint8_t  currentRotation   = ROT_LANDSCAPE_USB_RIGHT;
static uint8_t  candidateRotation = ROT_LANDSCAPE_USB_RIGHT;
static uint32_t candidateSinceMs  = 0;
static uint32_t lastSampleMs      = 0;
static bool     autoRotateEnabled = true;

// fade to black, swap rotation, fade back in so the redraw isn't jarring
static void do_rotation(uint8_t newRot) {

    int startBright = (int)os.brightness;
    for (int i = FADE_STEPS; i >= 0; i--) {
        int b = (startBright * i) / FADE_STEPS;
        M5.Display.setBrightness((uint8_t)b);
        delay(FADE_STEP_DELAY);
    }

    currentRotation = newRot;
    M5.Display.setRotation(newRot);
    os_force_redraw();
    gfx_flush();

    for (int i = 0; i <= FADE_STEPS; i++) {
        int b = (startBright * i) / FADE_STEPS;
        M5.Display.setBrightness((uint8_t)b);
        delay(FADE_STEP_DELAY);
    }

    M5.Display.setBrightness(os.brightness);

    os.dirty = false;
}

void rotation_init() {
    currentRotation   = ROT_LANDSCAPE_USB_RIGHT;
    candidateRotation = currentRotation;
    candidateSinceMs  = millis();
    lastSampleMs      = 0;
}

void rotation_set_auto(bool enabled) { autoRotateEnabled = enabled; }
bool rotation_get_auto()             { return autoRotateEnabled; }

void rotation_update() {
    if (!autoRotateEnabled) return;

    uint32_t now = millis();
    if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
    lastSampleMs = now;

    M5.Imu.update();
    auto imu = M5.Imu.getImuData();

    float ax = imu.accel.x;
    float ay = imu.accel.y;

    uint8_t desired = currentRotation;

    if (fabsf(ax) > fabsf(ay)) {
        if (fabsf(ax) >= TILT_THRESHOLD_G)
            desired = (ax > 0) ? ROT_LANDSCAPE_USB_LEFT : ROT_LANDSCAPE_USB_RIGHT;
    } else {
        if (fabsf(ay) >= TILT_THRESHOLD_G)
            desired = (ay > 0) ? ROT_PORTRAIT_USB_DOWN : ROT_PORTRAIT_USB_UP;
    }

    // require the tilt to hold steady for a bit before committing, otherwise
    // it flips back and forth when the tablet is near flat
    if (desired != candidateRotation) {
        candidateRotation = desired;
        candidateSinceMs  = now;
    }

    if (candidateRotation != currentRotation &&
        (now - candidateSinceMs) >= ROTATE_DEBOUNCE_MS) {
        do_rotation(candidateRotation);
    }
}

