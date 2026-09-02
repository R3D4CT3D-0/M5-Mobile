
#define GFX_DRIVER_INTERNAL
#include "gfx_driver.h"
#include "p4_os.h"

#include <esp_err.h>
#include <cstring>

#if __has_include(<esp_lcd_mipi_dsi.h>) && __has_include(<esp_lcd_panel_ops.h>) \
    && __has_include("lgfx/v1/platforms/esp32p4/Panel_DSI.hpp")
  #include <esp_lcd_mipi_dsi.h>
  #include <esp_lcd_panel_ops.h>
  #include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
  #define GFX_HAVE_DSI_FLIP 1
#else
  #define GFX_HAVE_DSI_FLIP 0
#endif

_GFXWrapper GFX;
static bool s_initialised = false;

static void dsi_force_resync_if_native(int w, int h);

namespace gfx_hw {
    void setBrightness(uint8_t b) {
        M5.Display.setBrightness(b);
    }
    void setRotation(uint8_t r) {
        M5.Display.setRotation(r);
        int nw = M5.Display.width(), nh = M5.Display.height();
        auto& sprite = GFX.Display;
        // resize the backing sprite to match the new orientation
        if (sprite.width() == 0 || nw != sprite.width() || nh != sprite.height()) {
            if (sprite.width() != 0) sprite.deleteSprite();
            sprite.setColorDepth(16);
            sprite.setPsram(true);
            void* buf = sprite.createSprite(nw, nh);
            Serial.printf("[gfx] setRotation(%u): createSprite(%d,%d) -> %p\n", r, nw, nh, buf);
        }
        sprite.markAllDirty();

        dsi_force_resync_if_native(nw, nh);
    }
    uint8_t getRotation() {
        return (uint8_t)M5.Display.getRotation();
    }
    int32_t hw_width()  { return M5.Display.width(); }
    int32_t hw_height() { return M5.Display.height(); }
}

#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
static const size_t GFX_PSRAM_CACHE_ALIGN = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
static const size_t GFX_PSRAM_CACHE_ALIGN = 128;
#endif

#if GFX_HAVE_PPA
namespace {
    ppa_client_handle_t s_ppaSrmClient  = nullptr;
    bool   s_ppaAvailable = false;
    size_t s_cacheAlign   = GFX_PSRAM_CACHE_ALIGN;

    inline size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

    void ppa_setup() {
        s_cacheAlign = GFX_PSRAM_CACHE_ALIGN;
        ppa_client_config_t srmCfg = {};
        srmCfg.oper_type = PPA_OPERATION_SRM;
        srmCfg.max_pending_trans_num = 1;
        esp_err_t e2 = ppa_register_client(&srmCfg, &s_ppaSrmClient);

        s_ppaAvailable = (e2 == ESP_OK);
        Serial.printf("[gfx] PPA SRM client: %s -> %s (cacheAlign=%u)\n",
                      esp_err_to_name(e2),
                      s_ppaAvailable ? "OK" : "UNAVAILABLE (falling back to software readRect)",
                      (unsigned)s_cacheAlign);
    }
}

bool gfx_hw::ppa_fill_rect(uint16_t* spriteBuf, int32_t spriteW, int32_t spriteH,
                            int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color24) {
    (void)spriteBuf; (void)spriteW; (void)spriteH;
    (void)x; (void)y; (void)w; (void)h; (void)color24;
    return false;
}

// hardware-accelerated rect copy, used instead of a software readRect when the PPA is available
bool gfx_hw::ppa_copy_rect(const uint16_t* spriteBuf, int32_t spriteW, int32_t spriteH,
                            int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* dstBuf) {
    if (!s_ppaAvailable || !spriteBuf || !dstBuf) return false;

    ppa_in_pic_blk_config_t in = {};
    in.buffer         = spriteBuf;
    in.pic_w          = spriteW;
    in.pic_h          = spriteH;
    in.block_w        = w;
    in.block_h        = h;
    in.block_offset_x = x;
    in.block_offset_y = y;
    in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    ppa_out_pic_blk_config_t out = {};
    out.buffer         = dstBuf;
    out.buffer_size    = align_up((size_t)w * h * sizeof(uint16_t), s_cacheAlign);
    out.pic_w          = w;
    out.pic_h          = h;
    out.block_offset_x = 0;
    out.block_offset_y = 0;
    out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    ppa_srm_oper_config_t cfg = {};
    cfg.in             = in;
    cfg.out            = out;
    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x        = 1.0f;
    cfg.scale_y        = 1.0f;
    cfg.mirror_x       = false;
    cfg.mirror_y       = false;
    cfg.mode           = PPA_TRANS_MODE_BLOCKING;

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppaSrmClient, &cfg);
    return err == ESP_OK;
}
#else
bool gfx_hw::ppa_fill_rect(uint16_t*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t) { return false; }
bool gfx_hw::ppa_copy_rect(const uint16_t*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint16_t*) { return false; }
#endif

#if GFX_HAVE_DSI_FLIP
namespace {
    esp_lcd_panel_handle_t s_dsiPanel   = nullptr;
    uint16_t*              s_fb[2]      = { nullptr, nullptr };
    int                     s_frontIdx  = 0;
    uint64_t                s_bufDirty[2] = { ~0ULL, ~0ULL };
    bool                    s_dsiReady  = false;
    int32_t                 s_nativeW   = 0;
    int32_t                 s_nativeH   = 0;

    bool dsi_flip_init() {
        auto* panel = M5.Display.getPanel();
        if (!panel) return false;

        auto* dsiPanel = reinterpret_cast<lgfx::Panel_DSI*>(panel);
        s_dsiPanel = dsiPanel->getDsiPanelHandle();
        if (!s_dsiPanel) return false;

        s_nativeW = M5.Display.width();
        s_nativeH = M5.Display.height();

        void* fb0 = nullptr; void* fb1 = nullptr;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_dsiPanel, 2, &fb0, &fb1) != ESP_OK
            || !fb0 || !fb1) {
            Serial.println("[gfx] DSI: couldn't get both hardware framebuffers — falling back to single-buffer push path.");
            return false;
        }
        s_fb[0] = (uint16_t*)fb0;
        s_fb[1] = (uint16_t*)fb1;
        s_frontIdx = 0;
        s_bufDirty[0] = s_bufDirty[1] = ~0ULL;
        Serial.printf("[gfx] DSI double-buffer flip enabled: fb0=%p fb1=%p  native=%dx%d\n",
                      fb0, fb1, s_nativeW, s_nativeH);
        return true;
    }

    // copies only the dirty tiles from the sprite into one hardware framebuffer,
    // swapping byte order since the DSI panel wants big-endian RGB565
    void dsi_sync_back_buffer(int backIdx, const uint16_t* spriteBuf, int W, int H) {
        uint64_t backMask = s_bufDirty[backIdx];
        if (backMask == 0) return;
        int tw = W / GFX_TILE_COLS, th = H / GFX_TILE_ROWS;
        uint16_t* dst = s_fb[backIdx];

        // dirty mask is one bit per tile; walk each row and copy contiguous runs of dirty tiles at once
        for (int r = 0; r < GFX_TILE_ROWS; r++) {
            int c = 0;
            while (c < GFX_TILE_COLS) {
                if (!(backMask & (1ULL << (r * GFX_TILE_COLS + c)))) { c++; continue; }
                int runStart = c++;
                while (c < GFX_TILE_COLS && (backMask & (1ULL << (r * GFX_TILE_COLS + c)))) c++;
                int px = runStart * tw, py = r * th;
                int pw = (c - runStart) * tw, ph = th;
                if (px + pw > W) pw = W - px;
                if (py + ph > H) ph = H - py;
                if (pw <= 0 || ph <= 0) continue;
                for (int yy = 0; yy < ph; yy++) {
                    const uint16_t* srcRow = spriteBuf + (py + yy) * W + px;
                    uint16_t*       dstRow = dst        + (py + yy) * W + px;
                    for (int xx = 0; xx < pw; xx++) {
                        dstRow[xx] = __builtin_bswap16(srcRow[xx]);
                    }
                }
            }
        }
        s_bufDirty[backIdx] = 0;
    }
}

// forces a full repaint of both hardware framebuffers, used after a rotation
// swap so the previously off-screen buffer isn't left showing stale content
static void dsi_force_resync_if_native(int w, int h) {
#if GFX_HAVE_DSI_FLIP
    if (!s_dsiReady || w != s_nativeW || h != s_nativeH) return;
    auto& sprite = GFX.Display;
    if (sprite.width() != w || sprite.height() != h) return;
    const uint16_t* spriteBuf = (const uint16_t*)sprite.getBuffer();
    if (!spriteBuf) return;

    s_bufDirty[0] = s_bufDirty[1] = ~0ULL;
    dsi_sync_back_buffer(0, spriteBuf, w, h);
    dsi_sync_back_buffer(1, spriteBuf, w, h);

    esp_lcd_panel_draw_bitmap(s_dsiPanel, 0, 0, w, h, s_fb[0]);
    s_frontIdx = 0;

    Serial.println("[gfx] DSI resync: forced both framebuffers to current content, selected buffer 0");
#else
    (void)w; (void)h;
#endif
}
#endif

// exposes the raw framebuffers so the BSOD handler can draw directly to the
// screen from a crash context, bypassing the normal sprite/flush pipeline
IRAM_ATTR gfx_panic_fb_t gfx_get_panic_framebuffers() {
    gfx_panic_fb_t r{};
#if GFX_HAVE_DSI_FLIP
    r.fb[0] = s_fb[0];
    r.fb[1] = s_fb[1];
    r.w     = s_nativeW;
    r.h     = s_nativeH;
    r.valid = s_dsiReady && s_fb[0] && s_fb[1] && s_nativeW > 0 && s_nativeH > 0;
#else
    r.fb[0] = r.fb[1] = nullptr;
    r.w = r.h = 0;
    r.valid = false;
#endif
    return r;
}

IRAM_ATTR void gfx_panic_flush_framebuffer(int idx) {
#if GFX_HAVE_DSI_FLIP && GFX_HAVE_CACHE_MSYNC
    if (idx != 0 && idx != 1) return;
    if (!s_fb[idx] || s_nativeW <= 0 || s_nativeH <= 0) return;

    esp_cache_msync((void*)s_fb[idx],
                     (size_t)s_nativeW * s_nativeH * sizeof(uint16_t),
                     ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#else
    (void)idx;
#endif
}

static const int    RING_SLOTS      = 16;
static const int GROUND_STRIP_MAX_H = 90 + 46;
static const size_t TILE_BUF_BYTES  =
    ((((size_t)SCREEN_WIDTH > SCREEN_HEIGHT ? SCREEN_WIDTH : SCREEN_HEIGHT)
      * (size_t)GROUND_STRIP_MAX_H * sizeof(uint16_t)
      + GFX_PSRAM_CACHE_ALIGN - 1) & ~(GFX_PSRAM_CACHE_ALIGN - 1));

struct TileSlot {
    uint16_t*        buf;
    volatile bool    inFlight;
};
static TileSlot s_ring[RING_SLOTS];
static int      s_ringHead = 0;

// ring of PSRAM tile buffers used to hand pixel data off to the flush task
// without blocking the caller on the SPI/parallel bus
static void ring_init() {
    for (int i = 0; i < RING_SLOTS; i++) {
        s_ring[i].buf      = (uint16_t*)heap_caps_aligned_alloc(GFX_PSRAM_CACHE_ALIGN, TILE_BUF_BYTES, MALLOC_CAP_SPIRAM);
        s_ring[i].inFlight = false;
        if (!s_ring[i].buf) {
            Serial.printf("[gfx] FATAL: ring slot %d alloc failed (%u bytes PSRAM)\n", i, (unsigned)TILE_BUF_BYTES);
        }
    }
}

static TileSlot* ring_acquire() {
    TileSlot* slot = &s_ring[s_ringHead];
    s_ringHead = (s_ringHead + 1) & (RING_SLOTS - 1);
    while (slot->inFlight) { vTaskDelay(1); }
    slot->inFlight = true;
    return slot;
}

struct FlushJob {
    int16_t   x, y, w, h;
    TileSlot* slot;
};

static QueueHandle_t s_flushQueue      = nullptr;
static TaskHandle_t  s_flushTaskHandle = nullptr;
static SemaphoreHandle_t s_busMutex    = nullptr;

// runs on its own core; drains queued tile jobs and pushes them to the panel,
// draining the whole queue before releasing the bus so writes don't interleave
static void gfx_flush_task(void*) {
    FlushJob job;
    for (;;) {
        if (xQueueReceive(s_flushQueue, &job, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_busMutex, portMAX_DELAY);
        M5.Display.startWrite();
        for (;;) {
            M5.Display.pushImage(job.x, job.y, job.w, job.h, job.slot->buf);
            job.slot->inFlight = false;
            if (xQueueReceive(s_flushQueue, &job, 0) != pdTRUE) break;
        }
        M5.Display.endWrite();
        xSemaphoreGive(s_busMutex);
    }
}

// used on boards without the DSI double-buffer path: reads only the dirty
// tiles out of the sprite and queues them for the flush task
static void fallback_flush(uint64_t mask) {
    auto& sprite = GFX.Display;
    int W = sprite.width(), H = sprite.height();
    int tw = W / GFX_TILE_COLS, th = H / GFX_TILE_ROWS;
    const uint16_t* spriteBuf = (const uint16_t*)sprite.getBuffer();

    for (int r = 0; r < GFX_TILE_ROWS; r++) {
        int c = 0;
        while (c < GFX_TILE_COLS) {
            if (!(mask & (1ULL << (r * GFX_TILE_COLS + c)))) { c++; continue; }
            int runStart = c++;
            while (c < GFX_TILE_COLS && (mask & (1ULL << (r * GFX_TILE_COLS + c)))) c++;
            int px = runStart * tw, py = r * th;
            int pw = (c - runStart) * tw, ph = th;
            if (px + pw > W) pw = W - px;
            if (py + ph > H) ph = H - py;
            if (pw <= 0 || ph <= 0) continue;

            TileSlot* slot = ring_acquire();
            if (!gfx_hw::ppa_copy_rect(spriteBuf, W, H, px, py, pw, ph, slot->buf)) {
                sprite.readRect(px, py, pw, ph, slot->buf);
            }
            FlushJob job{ (int16_t)px, (int16_t)py, (int16_t)pw, (int16_t)ph, slot };
            if (xQueueSend(s_flushQueue, &job, 0) != pdTRUE) {
                slot->inFlight = false;
            }
        }
    }
}

void gfx_flush_wait() {
    if (!s_initialised) return;
#if GFX_HAVE_DSI_FLIP
    if (s_dsiReady && GFX.Display.width() == s_nativeW && GFX.Display.height() == s_nativeH)
        return;
#endif
    for (int i = 0; i < RING_SLOTS; i++) {
        while (s_ring[i].inFlight) { vTaskDelay(1); }
    }
}

void gfx_init() {
    int W = M5.Display.width();
    int H = M5.Display.height();

    GFX.Display.setColorDepth(16);
    GFX.Display.setPsram(true);
    void* buf = GFX.Display.createSprite(W, H);

    Serial.printf("[gfx] createSprite(%d,%d) -> %p  freeHeap=%u freePsram=%u  "
                  "sprite.width()=%d sprite.height()=%d\n",
                  W, H, buf,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                  GFX.Display.width(), GFX.Display.height());

    if (!buf) {
        Serial.println("[gfx] FATAL: sprite allocation failed — check PSRAM is enabled.");
    }

#if GFX_HAVE_PPA
    ppa_setup();
#else
    Serial.println("[gfx] <driver/ppa.h> not found — 2D accelerator disabled, using software path.");
#endif

    GFX.Display.fillScreen(TFT_BLACK);
    GFX.Display.markAllDirty();

#if GFX_HAVE_DSI_FLIP
    s_dsiReady = dsi_flip_init();

    Serial.printf("[gfx] DSI flip path: %s  |  BSOD cache-flush: %s\n",
                  s_dsiReady ? "ENABLED" : "DISABLED (fallback)",
                  (GFX_HAVE_DSI_FLIP && GFX_HAVE_CACHE_MSYNC) ? "OK" : "*** NO-OP — board package too old ***");
#endif

    bool needFallback = true;
    if (needFallback) {
        ring_init();
        s_busMutex   = xSemaphoreCreateMutex();
        s_flushQueue = xQueueCreate(RING_SLOTS, sizeof(FlushJob));
        xTaskCreatePinnedToCore(gfx_flush_task, "gfx_flush", 4096, nullptr,
                                 5, &s_flushTaskHandle, 0 );
    }

    Serial.printf("[gfx] init: hw=%dx%d, mode=%s\n", W, H,
#if GFX_HAVE_DSI_FLIP
                  s_dsiReady ? "DSI-double-buffer-flip" : "fallback-tile-push"
#else
                  "fallback-tile-push"
#endif
                  );
    s_initialised = true;
}

int gfx_flush() {
    if (!s_initialised) return 0;
    auto& sprite = GFX.Display;
    uint64_t mask = sprite.getDirty();
    sprite.clearDirty();

#if GFX_HAVE_DSI_FLIP
    bool nativeOrientation = s_dsiReady &&
        sprite.width() == s_nativeW && sprite.height() == s_nativeH;
    if (nativeOrientation) {
        if (mask == 0 && s_bufDirty[1 - s_frontIdx] == 0) return 0;

        s_bufDirty[0] |= mask;
        s_bufDirty[1] |= mask;

        int backIdx = 1 - s_frontIdx;
        if (s_bufDirty[backIdx] == 0) return 0;

        int W = sprite.width(), H = sprite.height();
        const uint16_t* spriteBuf = (const uint16_t*)sprite.getBuffer();
        dsi_sync_back_buffer(backIdx, spriteBuf, W, H);

        esp_lcd_panel_draw_bitmap(s_dsiPanel, 0, 0, W, H, s_fb[backIdx]);
        s_frontIdx = backIdx;
        return 1;
    }
#endif

    if (mask == 0) return 0;
    fallback_flush(mask);
    return 1;
}

static inline bool gfx_clip_to_native(int& x, int& y, int& w, int& h,
                                       int& srcOffX, int& srcOffY) {
    srcOffX = 0; srcOffY = 0;
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { srcOffX = -x; w += x; x = 0; }
    if (y < 0) { srcOffY = -y; h += y; y = 0; }
    if (x >= s_nativeW || y >= s_nativeH || w <= 0 || h <= 0) return false;
    if (x + w > s_nativeW) w = s_nativeW - x;
    if (y + h > s_nativeH) h = s_nativeH - y;
    return (w > 0 && h > 0);
}

void gfx_push_direct(int x, int y, int w, int h, const uint16_t* buf) {
    if (!s_initialised || !buf || w <= 0 || h <= 0) return;

#if GFX_HAVE_DSI_FLIP
    if (s_dsiReady && GFX.Display.width() == s_nativeW && GFX.Display.height() == s_nativeH) {
        const int origW = w;
        int cx = x, cy = y, cw = w, ch = h, offX = 0, offY = 0;
        if (!gfx_clip_to_native(cx, cy, cw, ch, offX, offY)) return;
        for (int side = 0; side < 2; side++) {
            uint16_t* dst = s_fb[side];
            for (int yy = 0; yy < ch; yy++) {
                const uint16_t* srcRow = buf + (size_t)(offY + yy) * origW + offX;
                uint16_t*       dstRow = dst + (size_t)(cy + yy) * s_nativeW + cx;
                for (int xx = 0; xx < cw; xx++)
                    dstRow[xx] = __builtin_bswap16(srcRow[xx]);
            }
        }
        return;
    }
#endif

    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    if (bytes > TILE_BUF_BYTES) {
        Serial.printf("[gfx] gfx_push_direct: %dx%d too large for ring slot, dropping\n", w, h);
        return;
    }
    TileSlot* slot = ring_acquire();
    memcpy(slot->buf, buf, bytes);
    FlushJob job{ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, slot };
    if (xQueueSend(s_flushQueue, &job, 0) != pdTRUE) {
        slot->inFlight = false;
    }
}
void gfx_push_direct_noswap(int x, int y, int w, int h, const uint16_t* buf) {
    if (!s_initialised || !buf || w <= 0 || h <= 0) return;

#if GFX_HAVE_DSI_FLIP
    if (s_dsiReady && GFX.Display.width() == s_nativeW && GFX.Display.height() == s_nativeH) {
        const int origW = w;
        int cx = x, cy = y, cw = w, ch = h, offX = 0, offY = 0;
        if (!gfx_clip_to_native(cx, cy, cw, ch, offX, offY)) return;
        for (int side = 0; side < 2; side++) {
            uint16_t* dst = s_fb[side];
            for (int yy = 0; yy < ch; yy++) {
                const uint16_t* srcRow = buf + (size_t)(offY + yy) * origW + offX;
                uint16_t*       dstRow = dst + (size_t)(cy + yy) * s_nativeW + cx;
                memcpy(dstRow, srcRow, (size_t)cw * sizeof(uint16_t));
            }
        }
        return;
    }
#endif

    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    if (bytes > TILE_BUF_BYTES) {
        Serial.printf("[gfx] gfx_push_direct_noswap: %dx%d too large for ring slot, dropping\n", w, h);
        return;
    }
    TileSlot* slot = ring_acquire();
    memcpy(slot->buf, buf, bytes);
    FlushJob job{ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, slot };
    if (xQueueSend(s_flushQueue, &job, 0) != pdTRUE) {
        slot->inFlight = false;
    }
}
