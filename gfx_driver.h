#ifndef GFX_DRIVER_H
#define GFX_DRIVER_H
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>

#if __has_include(<driver/ppa.h>)
  #include <driver/ppa.h>
  #define GFX_HAVE_PPA 1
#else
  #define GFX_HAVE_PPA 0
#endif

#if __has_include(<esp_cache.h>)
  #include <esp_cache.h>
  #define GFX_HAVE_CACHE_MSYNC 1
#else
  #define GFX_HAVE_CACHE_MSYNC 0
#endif

static const int GFX_TILE_COLS = 8;
static const int GFX_TILE_ROWS = 8;

static const int GFX_PPA_FILL_MIN_AREA = 4096;

namespace gfx_hw {
    void setBrightness(uint8_t b);
    void setRotation(uint8_t r);
    uint8_t getRotation();
    int32_t hw_width();
    int32_t hw_height();

    bool ppa_fill_rect(uint16_t* spriteBuf, int32_t spriteW, int32_t spriteH,
                        int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color24);

    bool ppa_copy_rect(const uint16_t* spriteBuf, int32_t spriteW, int32_t spriteH,
                        int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* dstBuf);
}

class DirtySprite : public LGFX_Sprite {
public:
    DirtySprite() : LGFX_Sprite(), _dirty(0) {}

    // marks every tile touched by this rect as dirty so gfx_flush only pushes changed regions
    void markDirty(int32_t x, int32_t y, int32_t w, int32_t h) {
        if (w <= 0 || h <= 0) return;
        int sw = LGFX_Sprite::width();
        int sh = LGFX_Sprite::height();
        if (sw <= 0 || sh <= 0) return;
        int tw = sw / GFX_TILE_COLS;
        int th = sh / GFX_TILE_ROWS;
        if (tw <= 0 || th <= 0) return;
        int x2 = x + w - 1, y2 = y + h - 1;
        if (x < 0) x = 0;  if (y < 0) y = 0;
        if (x2 >= sw) x2 = sw - 1;
        if (y2 >= sh) y2 = sh - 1;
        if (x > x2 || y > y2) return;
        for (int r = y/th; r <= y2/th && r < GFX_TILE_ROWS; r++)
            for (int c = x/tw; c <= x2/tw && c < GFX_TILE_COLS; c++)
                _dirty |= (1ULL << (r * GFX_TILE_COLS + c));
    }
    void markAllDirty() { _dirty = ~0ULL; }
    void clearDirty()   { _dirty = 0; }
    uint64_t getDirty() const { return _dirty; }

    void fillScreen(uint32_t color) {
        markAllDirty();
        int w = LGFX_Sprite::width(), h = LGFX_Sprite::height();
        if (!gfx_hw::ppa_fill_rect((uint16_t*)LGFX_Sprite::getBuffer(), w, h, 0, 0, w, h, color))
            LGFX_Sprite::fillScreen(color);
    }
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
        markDirty(x,y,w,h);
        if ((int64_t)w * h < GFX_PPA_FILL_MIN_AREA ||
            !gfx_hw::ppa_fill_rect((uint16_t*)LGFX_Sprite::getBuffer(),
                                    LGFX_Sprite::width(), LGFX_Sprite::height(),
                                    x, y, w, h, color))
            LGFX_Sprite::fillRect(x,y,w,h,color);
    }
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
        { markDirty(x,y,w,h); LGFX_Sprite::fillRoundRect(x,y,w,h,r,color); }
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
        { markDirty(x,y,w,h); LGFX_Sprite::drawRect(x,y,w,h,color); }
    void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
        { markDirty(x,y,w,h); LGFX_Sprite::drawRoundRect(x,y,w,h,r,color); }
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color)
        { markDirty(x,y,w,1); LGFX_Sprite::drawFastHLine(x,y,w,color); }
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color)
        { markDirty(x,y,1,h); LGFX_Sprite::drawFastVLine(x,y,h,color); }
    void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color)
        { markDirty(x-r,y-r,r*2+1,r*2+1); LGFX_Sprite::fillCircle(x,y,r,color); }
    void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color)
        { markDirty(x-r,y-r,r*2+1,r*2+1); LGFX_Sprite::drawCircle(x,y,r,color); }

    void drawString(const char* str, int32_t x, int32_t y) {
        int fh = (int)fontHeight();
        int estW = (int)textWidth(str);
        int ax = x, ay = y;
        auto datum = getTextDatum();
        if (datum==middle_center||datum==top_center   ||datum==bottom_center) ax -= estW/2;
        if (datum==middle_right ||datum==top_right    ||datum==bottom_right)  ax -= estW;
        if (datum==middle_center||datum==middle_left  ||datum==middle_right)  ay -= fh/2;
        if (datum==bottom_center||datum==bottom_left  ||datum==bottom_right)  ay -= fh;
        markDirty(ax-2, ay-2, estW+8, fh+8);
        LGFX_Sprite::drawString(str, x, y);
    }

    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data)
        { markDirty(x,y,w,h); LGFX_Sprite::pushImage(x,y,w,h,data); }

    void drawString(const String& str, int32_t x, int32_t y) {
        drawString(str.c_str(), x, y);
    }

    void    setBrightness(uint8_t b)  { gfx_hw::setBrightness(b); }
    void    setRotation(uint8_t r)    { gfx_hw::setRotation(r); }
    uint8_t getRotation() const       { return gfx_hw::getRotation(); }

private:
    uint64_t _dirty;
};

struct _GFXWrapper {
    DirtySprite Display;

    decltype(::M5.Touch)& Touch  = ::M5.Touch;
    decltype(::M5.Imu)&   Imu    = ::M5.Imu;
    decltype(::M5.Power)& Power  = ::M5.Power;
    decltype(::M5.Rtc)&   Rtc    = ::M5.Rtc;

    void update() { ::M5.update(); }
    void begin(m5::M5Unified::config_t cfg = m5::M5Unified::config_t()) { ::M5.begin(cfg); }
    m5::M5Unified::config_t config() { return ::M5.config(); }

    operator m5::M5Unified&() { return ::M5; }
};

extern _GFXWrapper GFX;

void gfx_init();
int  gfx_flush();

void gfx_flush_wait();

// everywhere outside this driver, M5.Display should actually hit our dirty-tracking
// sprite instead of the raw display, so redirect the M5 symbol to our wrapper
#ifndef GFX_DRIVER_INTERNAL
#undef  M5
#define M5 GFX
#endif

struct gfx_panic_fb_t {
    uint16_t* fb[2];
    int32_t   w, h;
    bool      valid;
};
gfx_panic_fb_t gfx_get_panic_framebuffers();

void gfx_panic_flush_framebuffer(int idx);

void gfx_push_direct(int x, int y, int w, int h, const uint16_t* buf);

#endif

void gfx_push_direct_noswap(int x, int y, int w, int h, const uint16_t* buf);

