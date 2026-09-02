#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class DcStatus : uint8_t {
    OFFLINE  = 0,
    ONLINE   = 1,
    IDLE     = 2,
    DND      = 3,
    UNKNOWN  = 4,
};

enum class DcActivityType : uint8_t {
    NONE      = 0,
    PLAYING   = 0,
    STREAMING = 1,
    LISTENING = 2,
    WATCHING  = 3,
    CUSTOM    = 4,
    COMPETING = 5,
};

#define DC_MAX_DMS       32
#define DC_NAME_LEN      64
#define DC_ID_LEN        24
#define DC_AVATAR_LEN    48
#define DC_STATUS_LEN    80

struct DcDmEntry {
    char    channelId[DC_ID_LEN];
    char    userId[DC_ID_LEN];
    char    username[DC_NAME_LEN];
    char    globalName[DC_NAME_LEN];
    char    avatarHash[DC_AVATAR_LEN];
    char    lastMessageId[DC_ID_LEN];
    DcStatus       status;
    DcActivityType activityType;
    char    activityText[DC_STATUS_LEN];
    bool    valid;
};

extern DcDmEntry      dc_dms[DC_MAX_DMS];
extern int            dc_dm_count;
extern SemaphoreHandle_t dc_mutex;

enum class DcGwState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    IDENTIFYING,
    READY,
    ERROR,
};
extern volatile DcGwState dc_gw_state;

void dc_gateway_start(const char* token);

void dc_gateway_stop();

void dc_gateway_suspend();
void dc_gateway_resume();

// dc_dms is written by the gateway's background task and read by the UI task,
// so any access to it (or the fields above) must hold dc_mutex
inline void dc_gw_lock()   { xSemaphoreTake(dc_mutex, portMAX_DELAY); }
inline void dc_gw_unlock() { xSemaphoreGive(dc_mutex); }

// the gateway websocket and the plain HTTP calls (avatar/message fetches) share
// one TLS stack, which isn't safe to use from two tasks at once — serialize with this
extern SemaphoreHandle_t dc_tls_mutex;
inline void dc_tls_lock()   { if (dc_tls_mutex) xSemaphoreTake(dc_tls_mutex, portMAX_DELAY); }
inline void dc_tls_unlock() { xSemaphoreGive(dc_tls_mutex); }

uint32_t dc_status_colour(DcStatus s);

