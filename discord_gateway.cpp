#include "discord_gateway.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

DcDmEntry         dc_dms[DC_MAX_DMS];
int               dc_dm_count   = 0;
SemaphoreHandle_t dc_mutex      = nullptr;
SemaphoreHandle_t dc_tls_mutex  = nullptr;
volatile DcGwState dc_gw_state  = DcGwState::DISCONNECTED;

static char          s_token[512]          = "";
static TaskHandle_t  s_task               = nullptr;
static volatile bool s_running            = false;

static uint32_t      s_heartbeat_interval = 41250;
static uint32_t      s_last_heartbeat_ms  = 0;
static int32_t       s_seq               = -1;
static bool          s_heartbeat_acked   = true;
static bool          s_identified        = false;

static WiFiClientSecure* s_client        = nullptr;

uint32_t dc_status_colour(DcStatus s) {
    switch (s) {
        case DcStatus::ONLINE:  return 0x23A559U;
        case DcStatus::IDLE:    return 0xF0B232U;
        case DcStatus::DND:     return 0xF23F43U;
        default:                return 0x80848EU;
    }
}

static DcStatus parse_status(const char* s) {
    if (!s) return DcStatus::UNKNOWN;
    if (strcmp(s, "online")   == 0) return DcStatus::ONLINE;
    if (strcmp(s, "idle")     == 0) return DcStatus::IDLE;
    if (strcmp(s, "dnd")      == 0) return DcStatus::DND;
    return DcStatus::OFFLINE;
}

static DcDmEntry* find_or_create_dm(const char* userId) {
    for (int i = 0; i < dc_dm_count; i++)
        if (strcmp(dc_dms[i].userId, userId) == 0) return &dc_dms[i];
    if (dc_dm_count >= DC_MAX_DMS) return nullptr;
    DcDmEntry* e = &dc_dms[dc_dm_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->userId, DC_ID_LEN, "%s", userId);
    e->status = DcStatus::UNKNOWN;
    e->valid  = true;
    return e;
}

static DcDmEntry* find_dm(const char* userId) {
    for (int i = 0; i < dc_dm_count; i++)
        if (strcmp(dc_dms[i].userId, userId) == 0) return &dc_dms[i];
    return nullptr;
}

// minimal RFC6455 websocket frame writer — client-to-server frames must be masked,
// so XOR the payload with a random 4-byte key before sending
static bool ws_send_text(const char* payload) {
    if (!s_client || !s_client->connected()) return false;
    size_t plen = strlen(payload);

    uint8_t header[10];
    int hlen = 0;
    header[hlen++] = 0x81;

    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    if (plen <= 125) {
        header[hlen++] = 0x80 | (uint8_t)plen;
    } else if (plen <= 65535) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (plen >> 8) & 0xFF;
        header[hlen++] = plen & 0xFF;
    } else {
        return false;
    }

    header[hlen++] = mask[0]; header[hlen++] = mask[1];
    header[hlen++] = mask[2]; header[hlen++] = mask[3];

    dc_tls_lock();
    s_client->write(header, hlen);

    const uint8_t* src = (const uint8_t*)payload;
    uint8_t buf[256];
    size_t sent = 0;
    while (sent < plen) {
        size_t chunk = min(plen - sent, sizeof(buf));
        for (size_t i = 0; i < chunk; i++)
            buf[i] = src[sent + i] ^ mask[(sent + i) & 3];
        s_client->write(buf, chunk);
        sent += chunk;
    }
    dc_tls_unlock();
    return true;
}

// reads one logical websocket message, transparently reassembling it if the
// server split it across multiple continuation frames
static bool ws_recv_frame(char* buf, size_t bufSize, uint32_t timeoutMs = 60000) {
    if (!s_client) return false;

    uint32_t t0 = millis();
    size_t totalGot = 0;
    bool firstFrame = true;

    for (;;) {

        while (!s_client->available()) {
            if (!s_client->connected()) return false;
            if (millis() - t0 > timeoutMs) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        uint8_t h[2];
        int r = s_client->readBytes(h, 2);
        if (r != 2) return false;

        bool fin    = (h[0] & 0x80) != 0;
        uint8_t op  = h[0] & 0x0F;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t plen = h[1] & 0x7F;

        if (plen == 126) {
            uint8_t ext[2];
            if (s_client->readBytes(ext, 2) != 2) return false;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (s_client->readBytes(ext, 8) != 8) return false;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        uint8_t mask[4] = {0};
        if (masked) {
            if (s_client->readBytes(mask, 4) != 4) return false;
        }

        if (op == 0x8) {
            Serial.println("[GW] Server sent Close frame");
            return false;
        }
        // ping/pong keepalive frames — answer pings, discard both without exposing them to the caller
        if (op == 0x9 || op == 0xA) {
            if (op == 0x9) {
                uint8_t pong[2] = { 0x8A, 0x00 };
                s_client->write(pong, 2);
            }
            for (uint64_t i = 0; i < plen; i++) {
                while (!s_client->available()) vTaskDelay(pdMS_TO_TICKS(1));
                uint8_t b; s_client->readBytes(&b, 1);
            }
            continue;
        }

        size_t space   = (bufSize - 1 > totalGot) ? (bufSize - 1 - totalGot) : 0;
        size_t toRead  = (size_t)min(plen, (uint64_t)space);
        size_t got     = 0;
        while (got < toRead) {
            while (!s_client->available()) {
                if (!s_client->connected()) {
                    buf[totalGot + got] = '\0';
                    return (totalGot + got) > 0;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            int n = s_client->readBytes((uint8_t*)buf + totalGot + got, toRead - got);
            if (n > 0) got += n;
        }

        uint64_t remaining = plen - toRead;
        if (remaining > 0) {
            Serial.printf("[GW] WARNING: message exceeded frame buffer, dropping %llu bytes (buf=%u)\n",
                          (unsigned long long)remaining, (unsigned)bufSize);
        }
        while (remaining > 0) {
            while (!s_client->available()) vTaskDelay(pdMS_TO_TICKS(1));
            uint8_t sink; s_client->readBytes(&sink, 1);
            remaining--;
        }

        if (masked) {
            for (size_t i = 0; i < got; i++) buf[totalGot + i] ^= mask[i & 3];
        }

        totalGot += got;
        firstFrame = false;
        (void)firstFrame;

        if (fin) break;

    }

    buf[totalGot] = '\0';
    return totalGot > 0;
}

static void send_heartbeat() {
    char buf[64];
    if (s_seq >= 0) snprintf(buf, sizeof(buf), "{\"op\":1,\"d\":%d}", s_seq);
    else            snprintf(buf, sizeof(buf), "{\"op\":1,\"d\":null}");
    ws_send_text(buf);
    s_last_heartbeat_ms = millis();
    s_heartbeat_acked   = false;
    Serial.printf("[GW] Heartbeat sent (seq=%d)\n", s_seq);
}

// identify payload mimics the official desktop client's fingerprint, since the
// gateway is picky about what it accepts from unofficial connections
static void send_identify() {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"op\":2,\"d\":{"
          "\"token\":\"%s\","
          "\"properties\":{"
            "\"os\":\"Linux\","
            "\"browser\":\"Chrome\","
            "\"device\":\"\","
            "\"system_locale\":\"en-US\","
            "\"browser_user_agent\":\"Mozilla/5.0\","
            "\"browser_version\":\"120.0.0\","
            "\"os_version\":\"\","
            "\"referrer\":\"\","
            "\"referring_domain\":\"\","
            "\"release_channel\":\"stable\","
            "\"client_build_number\":0,"
            "\"client_event_source\":null"
          "},"
          "\"presence\":{\"status\":\"unknown\",\"since\":0,\"activities\":[],\"afk\":false},"
          "\"compress\":false,"
          "\"capabilities\":0"
        "}}",
        s_token);
    ws_send_text(buf);
    s_identified = true;
    Serial.println("[GW] Identify sent");
}

static void parse_activities(JsonArray acts, DcDmEntry* e) {
    e->activityType    = DcActivityType::NONE;
    e->activityText[0] = '\0';
    for (JsonObject act : acts) {
        int atype         = act["type"] | 0;
        const char* aname = act["name"]  | "";
        const char* state = act["state"] | "";
        if (atype == 4) {
            snprintf(e->activityText, DC_STATUS_LEN, "%s", state[0] ? state : aname);
            e->activityType = DcActivityType::CUSTOM;
            return;
        }
        if (atype == 2 && e->activityType == DcActivityType::NONE) {
            const char* details = act["details"] | "";
            snprintf(e->activityText, DC_STATUS_LEN, "%s%s%s",
                     aname, details[0] ? " \xe2\x80\x94 " : "", details);
            e->activityType = DcActivityType::LISTENING;
        }
        if (atype == 0 && e->activityType == DcActivityType::NONE) {
            snprintf(e->activityText, DC_STATUS_LEN, "%s", aname);
            e->activityType = DcActivityType::PLAYING;
        }
    }
}

// READY payloads can be huge, so ask ArduinoJson to only parse the fields we
// actually use instead of the full document
static void parse_ready(const char* json, size_t len) {
    DynamicJsonDocument* filter = new DynamicJsonDocument(2048);
    (*filter)["d"]["private_channels"][0]["id"]   = true;
    (*filter)["d"]["private_channels"][0]["type"] = true;
    (*filter)["d"]["private_channels"][0]["last_message_id"] = true;
    (*filter)["d"]["private_channels"][0]["recipients"][0]["id"]          = true;
    (*filter)["d"]["private_channels"][0]["recipients"][0]["username"]    = true;
    (*filter)["d"]["private_channels"][0]["recipients"][0]["global_name"] = true;
    (*filter)["d"]["private_channels"][0]["recipients"][0]["avatar"]      = true;
    (*filter)["d"]["presences"][0]["user"]["id"]              = true;
    (*filter)["d"]["presences"][0]["status"]                  = true;
    (*filter)["d"]["presences"][0]["activities"][0]["type"]   = true;
    (*filter)["d"]["presences"][0]["activities"][0]["name"]   = true;
    (*filter)["d"]["presences"][0]["activities"][0]["state"]  = true;
    (*filter)["d"]["presences"][0]["activities"][0]["details"]= true;

    DynamicJsonDocument* doc = new DynamicJsonDocument(32768);
    auto err = deserializeJson(*doc, json, len,
                                DeserializationOption::Filter(*filter),
                                DeserializationOption::NestingLimit(30));
    delete filter;
    if (err) {
        Serial.printf("[GW] READY parse error: %s\n", err.c_str());
        delete doc;
        return;
    }

    JsonObject d = (*doc)["d"];
    dc_gw_lock();

    struct Candidate {
        JsonObject ch;
        JsonObject recip;
        uint64_t   snowflake;
    };
    // only DC_MAX_DMS DM slots available, so keep a sorted top-N by most recent
    // message (snowflakes are time-ordered) via simple insertion rather than
    // sorting the whole (possibly huge) private_channels list
    Candidate top[DC_MAX_DMS];
    int topCount = 0;

    for (JsonObject ch : d["private_channels"].as<JsonArray>()) {
        if ((ch["type"] | -1) != 1) continue;
        JsonArray recips = ch["recipients"];
        if (recips.isNull() || recips.size() == 0) continue;
        JsonObject recip = recips[0];
        const char* uid = recip["id"] | "";
        if (!uid[0]) continue;

        const char* lastMsgStr = ch["last_message_id"] | "0";
        uint64_t snowflake = strtoull(lastMsgStr, nullptr, 10);

        if (topCount < DC_MAX_DMS) {
            int i = topCount++;
            while (i > 0 && top[i - 1].snowflake < snowflake) {
                top[i] = top[i - 1];
                i--;
            }
            top[i] = { ch, recip, snowflake };
        } else if (snowflake > top[DC_MAX_DMS - 1].snowflake) {
            int i = DC_MAX_DMS - 1;
            while (i > 0 && top[i - 1].snowflake < snowflake) {
                top[i] = top[i - 1];
                i--;
            }
            top[i] = { ch, recip, snowflake };
        }

    }

    for (int k = 0; k < topCount; k++) {
        JsonObject ch    = top[k].ch;
        JsonObject recip = top[k].recip;
        const char* uid  = recip["id"] | "";
        DcDmEntry* e = find_or_create_dm(uid);
        if (!e) continue;
        snprintf(e->channelId,  DC_ID_LEN,    "%s", ch["id"]             | "");
        snprintf(e->username,   DC_NAME_LEN,  "%s", recip["username"]    | "");
        snprintf(e->globalName, DC_NAME_LEN,  "%s", recip["global_name"] | "");
        snprintf(e->avatarHash, DC_AVATAR_LEN,"%s", recip["avatar"]      | "");
        snprintf(e->lastMessageId, DC_ID_LEN, "%s", ch["last_message_id"] | "0");
        e->valid = true;
    }

    for (JsonObject p : d["presences"].as<JsonArray>()) {
        const char* uid = p["user"]["id"] | "";
        if (!uid[0]) continue;
        DcDmEntry* e = find_dm(uid);
        if (!e) continue;
        e->status = parse_status(p["status"] | "offline");
        parse_activities(p["activities"], e);
    }

    dc_gw_state = DcGwState::READY;
    dc_gw_unlock();
    Serial.printf("[GW] READY: %d DMs\n", dc_dm_count);
    delete doc;
}

static void parse_presence_update(const char* json, size_t len) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, json, len)) return;
    JsonObject d   = doc["d"];
    const char* uid = d["user"]["id"] | "";
    if (!uid[0]) return;
    dc_gw_lock();
    DcDmEntry* e = find_dm(uid);
    if (e) {
        e->status = parse_status(d["status"] | "offline");
        parse_activities(d["activities"], e);
    }
    dc_gw_unlock();
}

// gateway opcodes: 10=Hello 11=HeartbeatACK 1=HeartbeatRequest 7=Reconnect
// 9=InvalidSession 0=Dispatch (an actual event, named in "t")
static bool dispatch(const char* p, size_t len) {
    const char* opPos = strstr(p, "\"op\":");
    if (!opPos) return true;
    int op = atoi(opPos + 5);

    switch (op) {
    case 10: {
        DynamicJsonDocument doc(512);
        deserializeJson(doc, p, len);
        s_heartbeat_interval = doc["d"]["heartbeat_interval"] | 41250;
        Serial.printf("[GW] Hello — interval=%ums\n", s_heartbeat_interval);
        send_heartbeat();
        send_identify();
        break;
    }
    case 11:
        s_heartbeat_acked = true;
        Serial.println("[GW] ACK");
        break;
    case 1:
        send_heartbeat();
        break;
    case 7:
        Serial.println("[GW] Reconnect requested");
        return false;
    case 9:
        Serial.println("[GW] Invalid Session");
        dc_gw_state = DcGwState::ERROR;
        return false;
    case 0: {
        const char* sPos = strstr(p, "\"s\":");
        if (sPos) s_seq = atoi(sPos + 4);
        const char* tPos = strstr(p, "\"t\":\"");
        if (!tPos) break;
        tPos += 5;
        if      (strncmp(tPos, "READY",           5)  == 0) { vTaskDelay(pdMS_TO_TICKS(1)); parse_ready(p, len); }
        else if (strncmp(tPos, "PRESENCE_UPDATE", 15) == 0) parse_presence_update(p, len);
        break;
    }
    default: break;
    }
    return true;
}

static bool ws_upgrade() {

    // the Sec-WebSocket-Key doesn't need to be random here — we never validate
    // the server's Sec-WebSocket-Accept, so a fixed nonce is fine
    dc_tls_lock();
    s_client->println("GET /?v=10&encoding=json HTTP/1.1");
    s_client->println("Host: gateway.discord.gg");
    s_client->println("Upgrade: websocket");
    s_client->println("Connection: Upgrade");
    s_client->println("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==");
    s_client->println("Sec-WebSocket-Version: 13");
    s_client->println("Origin: https://discord.com");
    s_client->println("User-Agent: DiscordBot (https://github.com/yourname/p4tabos, 1.0)");
    s_client->println();
    dc_tls_unlock();

    String status = s_client->readStringUntil('\n');
    if (!status.startsWith("HTTP/1.1 101")) {
        Serial.printf("[GW] Upgrade failed: %s\n", status.c_str());
        return false;
    }

    while (s_client->connected()) {
        String line = s_client->readStringUntil('\n');
        if (line == "\r" || line == "") break;
    }
    Serial.println("[GW] WebSocket upgrade OK");
    return true;
}

#define FRAME_BUF_SIZE (4 * 1024 * 1024)

static void gateway_task(void*) {
    Serial.println("[GW] Task started");

    char* frameBuf = (char*)heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frameBuf) {
        Serial.println("[GW] PSRAM alloc failed — task ending");
        dc_gw_state = DcGwState::ERROR;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint32_t retryDelay = 3000;

    // reconnect loop, run entirely on this dedicated task; retry with backoff
    // on any TCP/upgrade/protocol failure so the OS never has to babysit it
    while (s_running) {
        s_identified      = false;
        s_heartbeat_acked = true;
        s_seq             = -1;
        dc_gw_state       = DcGwState::CONNECTING;

        Serial.println("[GW] Connecting to gateway.discord.gg:443...");

        s_client = new WiFiClientSecure();
        s_client->setInsecure();
        s_client->setTimeout(15000);

        bool tcpOk = s_client->connect("gateway.discord.gg", 443);
        if (!tcpOk) {
            Serial.printf("[GW] TCP connect failed — retry in %ums\n", retryDelay);
            delete s_client; s_client = nullptr;
            for (uint32_t i = 0; i < retryDelay && s_running; i += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (retryDelay < 30000) retryDelay *= 2;
            continue;
        }
        Serial.println("[GW] TCP+TLS connected");

        if (!ws_upgrade()) {
            delete s_client; s_client = nullptr;
            for (uint32_t i = 0; i < retryDelay && s_running; i += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (retryDelay < 30000) retryDelay *= 2;
            continue;
        }

        retryDelay = 3000;
        dc_gw_state = DcGwState::IDENTIFYING;
        s_last_heartbeat_ms = millis();

        while (s_running && s_client->connected()) {

            uint32_t now = millis();
            if (s_identified && (now - s_last_heartbeat_ms >= s_heartbeat_interval)) {
                if (!s_heartbeat_acked) {
                    Serial.println("[GW] No ACK — zombie connection, reconnecting");
                    break;
                }
                send_heartbeat();
            }

            if (!s_client->available()) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (!ws_recv_frame(frameBuf, FRAME_BUF_SIZE, 5000)) {
                Serial.println("[GW] Frame read failed — reconnecting");
                break;
            }

            if (!dispatch(frameBuf, strlen(frameBuf))) {
                Serial.println("[GW] Dispatch requested disconnect");
                break;
            }

            if (dc_gw_state == DcGwState::ERROR) break;
        }

        if (s_client) { s_client->stop(); delete s_client; s_client = nullptr; }
        dc_gw_state = DcGwState::DISCONNECTED;

        if (s_running) {
            Serial.printf("[GW] Reconnecting in %ums...\n", retryDelay);
            for (uint32_t i = 0; i < retryDelay && s_running; i += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (retryDelay < 30000) retryDelay *= 2;
        }
    }

    heap_caps_free(frameBuf);
    if (s_client) { s_client->stop(); delete s_client; s_client = nullptr; }
    Serial.println("[GW] Task ending");
    dc_gw_state = DcGwState::DISCONNECTED;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void dc_gateway_start(const char* token) {

    if (s_task) dc_gateway_stop();

    if (!dc_mutex)     dc_mutex     = xSemaphoreCreateMutex();
    if (!dc_tls_mutex) dc_tls_mutex = xSemaphoreCreateMutex();
    snprintf(s_token, sizeof(s_token), "%s", token);
    s_running = true;

    dc_gw_lock();
    memset(dc_dms, 0, sizeof(dc_dms));
    dc_dm_count = 0;
    dc_gw_state = DcGwState::CONNECTING;
    dc_gw_unlock();

    xTaskCreatePinnedToCore(gateway_task, "dc_gateway", 12288, nullptr, 1, &s_task, 0);
}

void dc_gateway_suspend() {
    if (s_task) vTaskSuspend(s_task);
}

void dc_gateway_resume() {
    if (s_task) vTaskResume(s_task);
}

void dc_gateway_stop() {
    s_running = false;
    if (s_client) { s_client->stop(); }

    uint32_t t0 = millis();
    while (s_task != nullptr && (millis() - t0) < 2000) delay(50);
    if (s_task) { vTaskDelete(s_task); s_task = nullptr; }

    dc_gw_lock();
    memset(dc_dms, 0, sizeof(dc_dms));
    dc_dm_count = 0;
    dc_gw_state = DcGwState::DISCONNECTED;
    dc_gw_unlock();
}
