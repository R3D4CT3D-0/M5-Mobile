#pragma once

// bsod draws straight to the real M5.Display during a crash, so temporarily
// undo the gfx_driver's `#define M5 GFX` redirect before including headers that use M5
#ifdef M5
  #pragma push_macro("M5")
  #undef M5
  #define BSOD_HAD_M5_MACRO
#endif

#include <Arduino.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>
#include "esp_rom_sys.h"
#include "gfx_driver.h"
#include "bsod_font.h"

#ifdef BSOD_HAD_M5_MACRO
  #pragma pop_macro("M5")
  #undef BSOD_HAD_M5_MACRO
#endif

#define BSOD_BG     0x0015U
#define BSOD_FG     0xFFFFU
#define BSOD_ACCENT 0xFFE0U
#define BSOD_DIM     0xAD5FU
#define BSOD_BAR    0x0011U

struct _BsodReason {
    const char* tag;
    const char* headline;
    const char* detail[6];
};

static const _BsodReason _kReasons[] = {
    {
        "abort",
        "ABORT() / ASSERT FAILURE",
        {
            "The firmware called abort() or a configASSERT()/assert() check failed.",
            "The assert message and file:line appear on the serial log just before",
            "this crash. Common causes: bad parameter to an IDF API, a FreeRTOS",
            "function called from an ISR without the FromISR suffix, or an explicit",
            "abort() in app code.",
            NULL
        }
    },
    {
        "LoadProhibited",
        "NULL / UNMAPPED POINTER READ",
        {
            "The CPU tried to read from 0x00000000 or another unmapped region.",
            "Common causes: dereferencing an uninitialised or freed pointer, a stale",
            "reference after realloc(), or reading a member of a NULL struct.",
            "Check PC (crash address) and BAD ADDR (the invalid address) below.",
            NULL, NULL
        }
    },
    {
        "StoreProhibited",
        "NULL / UNMAPPED POINTER WRITE",
        {
            "The CPU tried to write to 0x00000000 or another unmapped region.",
            "Common causes: writing through a NULL pointer, a freed buffer, or",
            "writing past the end of a stack array into unmapped memory.",
            "Check PC and BAD ADDR below.",
            NULL, NULL
        }
    },
    {
        "InstrFetchProhibited",
        "BAD FUNCTION POINTER / CORRUPT RETURN ADDRESS",
        {
            "The CPU tried to fetch an instruction from an invalid address.",
            "Common causes: calling through a NULL/corrupt function pointer, a stack",
            "overflow that smashed the return address (RA), or jumping into data",
            "memory. If SP looks unusually low, stack overflow is likely.",
            "Also check for missing IRAM_ATTR on functions in ISR call chains.",
            NULL
        }
    },
    {
        "IllegalInstruction",
        "ILLEGAL INSTRUCTION",
        {
            "The CPU decoded an undefined opcode at the crash PC.",
            "Common causes: a FreeRTOS task function returned instead of calling",
            "vTaskDelete(); branching into a data section; or SPI flash pins",
            "reconfigured as GPIO while code is still fetching from flash.",
            NULL, NULL
        }
    },
    {
        "IntegerDivideByZero",
        "DIVISION BY ZERO",
        {
            "An integer divide-by-zero occurred. Add a zero-guard before that",
            "division. The crash PC points to the exact instruction.",
            NULL, NULL, NULL, NULL
        }
    },
    {
        "Stack canary",
        "STACK OVERFLOW (canary overwritten)",
        {
            "A FreeRTOS task overran its stack and corrupted the canary sentinel.",
            "The offending task name is in the serial log before this crash.",
            "Fix: increase that task's stack in xTaskCreatePinnedToCore(), or reduce",
            "its local variable usage / recursion depth.",
            "Tip: uxTaskGetStackHighWaterMark(NULL)==0 at runtime means you were",
            "already at the limit before the crash."
        }
    },
    {
        "OverFlow",
        "STACK OVERFLOW (FreeRTOS overflow hook)",
        {
            "vApplicationStackOverflowHook fired — a task's stack sentinel was gone.",
            "Task name is in the serial log. Increase its stack or reduce usage.",
            NULL, NULL, NULL, NULL
        }
    },
    {
        "Unhandled interrupt",
        "UNHANDLED INTERRUPT",
        {
            "An interrupt fired with no ISR registered for it.",
            "Common cause: a peripheral enabled before its IDF driver installs the",
            "ISR, or an ISR deregistered while the peripheral is still active.",
            NULL, NULL, NULL
        }
    },
    {
        "Cache disabled",
        "CACHE DISABLED DURING FLASH / PSRAM ACCESS",
        {
            "Code accessed flash or PSRAM while the cache was disabled.",
            "Every function in the ISR call chain must be marked IRAM_ATTR —",
            "including string literals, const arrays, and C++ vtable entries.",
            NULL, NULL, NULL
        }
    },
    {
        "Watchdog",
        "WATCHDOG TIMEOUT",
        {
            "The task watchdog (TWDT) or interrupt watchdog (IWDT) expired.",
            "TWDT: idle task was starved — a tight loop in another task with no",
            "vTaskDelay() / yield() / taskYIELD() call.",
            "IWDT: an ISR or scheduler-locked section ran longer than ~300 ms.",
            "Serial log will say 'Task watchdog got triggered' and name the task.",
            NULL
        }
    },
    {
        "wdt",
        "WATCHDOG TIMEOUT",
        {
            "A watchdog timer expired. Common cause: a loop that never yields to",
            "FreeRTOS, blocking I/O with no timeout, or an ISR that runs too long.",
            NULL, NULL, NULL, NULL
        }
    },
    {
        "heap",
        "HEAP CORRUPTION OR OUT-OF-MEMORY",
        {
            "The heap allocator detected corruption, a double-free, or fatal OOM.",
            "Common causes: writing past the end of a malloc'd buffer; double-free;",
            "use-after-free; or genuinely exhausting RAM.",
            "Add heap_caps_check_integrity_all(true) earlier to find the site.",
            NULL, NULL
        }
    },
    {
        "LoadStoreAlignment",
        "UNALIGNED MEMORY ACCESS",
        {
            "The CPU tried a multi-byte load/store at a misaligned address.",
            "Common cause: casting char*/uint8_t* to uint32_t* and dereferencing.",
            "Fix: use memcpy() to copy the bytes into a local aligned variable.",
            NULL, NULL, NULL
        }
    },
    {
        "Coprocessor",
        "FPU / COPROCESSOR EXCEPTION",
        {
            "A floating-point or coprocessor exception occurred.",
            "Most likely: FP operations inside an ISR without saving FPU context.",
            "Move FP work out of the ISR entirely.",
            NULL, NULL, NULL
        }
    },

    {
        "",
        "GURU MEDITATION / UNKNOWN EXCEPTION",
        {
            "An unrecognised exception or abort occurred.",
            "Key: PC = crash address, RA = return address (caller),",
            "SP = stack pointer, MCAUSE = exception number (RISC-V spec).",
            "Enable CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT in sdkconfig for a full",
            "register + backtrace dump on serial before the device resets.",
            NULL
        }
    },
};

static const int _kNR = (int)(sizeof(_kReasons) / sizeof(_kReasons[0]));

static IRAM_ATTR const _BsodReason* _bsod_find(const char* tag) {
    if (!tag) return &_kReasons[_kNR - 1];
    for (int i = 0; i < _kNR - 1; i++)
        if (_kReasons[i].tag[0] && strstr(tag, _kReasons[i].tag))
            return &_kReasons[i];
    return &_kReasons[_kNR - 1];
}

// everything below draws directly into a raw framebuffer with hand-rolled
// primitives (no sprite/font library calls) because this code runs from a
// panic handler where the heap and most drivers can no longer be trusted;
// IRAM_ATTR keeps it resident so it still works if flash access is broken
static IRAM_ATTR inline void _bsod_px(uint16_t* fb, int32_t W, int32_t H,
                                       int32_t x, int32_t y, uint16_t color) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    fb[(size_t)y * W + x] = __builtin_bswap16(color);
}

static IRAM_ATTR void _bsod_fillrect(uint16_t* fb, int32_t W, int32_t H,
                                      int32_t x, int32_t y, int32_t w, int32_t h,
                                      uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t swapped = __builtin_bswap16(color);
    for (int32_t yy = y; yy < y + h; yy++) {
        uint16_t* row = fb + (size_t)yy * W + x;
        for (int32_t xx = 0; xx < w; xx++) row[xx] = swapped;
    }
}

static IRAM_ATTR void _bsod_hline(uint16_t* fb, int32_t W, int32_t H,
                                   int32_t x, int32_t y, int32_t w, uint16_t color) {
    _bsod_fillrect(fb, W, H, x, y, w, 1, color);
}

static IRAM_ATTR void _bsod_char(uint16_t* fb, int32_t W, int32_t H,
                                  int32_t x, int32_t y, char ch,
                                  uint16_t fg, int32_t scale) {
    if (ch < BSOD_FONT_FIRST || ch > BSOD_FONT_LAST) return;
    const uint8_t* glyph = bsod_font[ch - BSOD_FONT_FIRST];
    for (int row = 0; row < BSOD_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < BSOD_FONT_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            if (scale == 1) {
                _bsod_px(fb, W, H, x + col, y + row, fg);
            } else {
                _bsod_fillrect(fb, W, H, x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
    }
}

static IRAM_ATTR int32_t _bsod_text(uint16_t* fb, int32_t W, int32_t H,
                                     int32_t x, int32_t y, int32_t maxW,
                                     const char* str, uint16_t fg, int32_t scale) {
    const int32_t cw = BSOD_FONT_W * scale;
    const int32_t lh = (BSOD_FONT_H + 4) * scale;
    int32_t cx = x, cy = y;
    for (const char* p = str; *p; p++) {
        if (*p == '\n' || (cx + cw > x + maxW && cx != x)) {
            cx = x; cy += lh;
            if (*p == '\n') continue;
        }
        _bsod_char(fb, W, H, cx, cy, *p, fg, scale);
        cx += cw;
    }
    return cy + lh;
}

static IRAM_ATTR void _bsod_draw_one(uint16_t* fb, int32_t W, int32_t H,
                                      const char* tag,
                                      uint32_t pc, uint32_t ra, uint32_t sp,
                                      uint32_t badaddr) {
    const _BsodReason* r = _bsod_find(tag);
    const int32_t ML = 24;

    _bsod_fillrect(fb, W, H, 0, 0, W, H, BSOD_BG);

    int32_t y = 20;
    _bsod_char(fb, W, H, ML, y, ':', BSOD_FG, 3);
    _bsod_char(fb, W, H, ML + BSOD_FONT_W*3, y, '(', BSOD_FG, 3);
    y += BSOD_FONT_H*3 + 16;

    y = _bsod_text(fb, W, H, ML, y, W - ML*2,
                    "P4-Tab OS ran into a problem and needs to restart.",
                    BSOD_FG, 1) + 10;

    y = _bsod_text(fb, W, H, ML, y, W - ML*2, r->headline, BSOD_ACCENT, 1) + 14;

    for (int i = 0; i < 6 && r->detail[i]; i++)
        y = _bsod_text(fb, W, H, ML, y, W - ML*2, r->detail[i], BSOD_FG, 1);

    y += 6;
    _bsod_hline(fb, W, H, ML, y, W - ML*2, BSOD_DIM);
    y += 14;

    static char buf[160];
    snprintf(buf, sizeof(buf), "PC: 0x%08X  RA: 0x%08X  SP: 0x%08X",
             (unsigned)pc, (unsigned)ra, (unsigned)sp);
    y = _bsod_text(fb, W, H, ML, y, W - ML*2, buf, BSOD_DIM, 1);

    snprintf(buf, sizeof(buf), "REASON: %s  BAD ADDR (mtval): 0x%08X",
             tag ? tag : "unknown", (unsigned)badaddr);
    y = _bsod_text(fb, W, H, ML, y, W - ML*2, buf, BSOD_DIM, 1);

    uint32_t s = millis() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %02u:%02u:%02u  Free heap: %u KB  Core: %d",
             (unsigned)(s/3600), (unsigned)((s%3600)/60), (unsigned)(s%60),
             (unsigned)(esp_get_free_heap_size() / 1024),
             (int)xPortGetCoreID());
    y = _bsod_text(fb, W, H, ML, y, W - ML*2, buf, BSOD_DIM, 1);

    static const char* kRR[] = {
        "Unknown","Power-on","External pin","Software",
        "Panic/Guru","Interrupt WDT","Task WDT","Other WDT",
        "Deep sleep","Brownout","SDIO"
    };
    esp_reset_reason_t rr = esp_reset_reason();
    snprintf(buf, sizeof(buf), "Previous reset: %s",
             ((unsigned)rr < 11) ? kRR[(int)rr] : "Unknown");
    _bsod_text(fb, W, H, ML, y, W - ML*2, buf, BSOD_DIM, 1);

    const int32_t barH = 40;
    _bsod_fillrect(fb, W, H, 0, H - barH, W, barH, BSOD_BAR);
    const char* footer = "Note the error above, then press reset to restart.";
    int32_t fw = (int32_t)strlen(footer) * BSOD_FONT_W;
    _bsod_text(fb, W, H, (W - fw) / 2, H - barH + (barH - BSOD_FONT_H)/2, W, footer, BSOD_FG, 1);
}

static IRAM_ATTR void _bsod_draw(const char* tag,
                                  uint32_t pc, uint32_t ra, uint32_t sp,
                                  uint32_t badaddr) {
    gfx_panic_fb_t pfb = gfx_get_panic_framebuffers();

    esp_rom_printf("BSOD: pfb.valid=%d fb[0]=%p fb[1]=%p w=%d h=%d\n",
                   (int)pfb.valid, pfb.fb[0], pfb.fb[1], (int)pfb.w, (int)pfb.h);

    if (pfb.valid) {

        esp_rom_printf("BSOD: drawing to raw framebuffers (%dx%d)\n", (int)pfb.w, (int)pfb.h);
        _bsod_draw_one(pfb.fb[0], pfb.w, pfb.h, tag, pc, ra, sp, badaddr);
        gfx_panic_flush_framebuffer(0);
        esp_rom_printf("BSOD: fb0 done + flushed\n");
        _bsod_draw_one(pfb.fb[1], pfb.w, pfb.h, tag, pc, ra, sp, badaddr);
        gfx_panic_flush_framebuffer(1);
        esp_rom_printf("BSOD: fb1 done + flushed\n");
        return;
    }

    esp_rom_printf("BSOD: no raw framebuffer available, falling back to M5GFX\n");
    __asm__ volatile ("csrw mie, %0" :: "r"(0xFFFFFFFFu));
    __asm__ volatile ("csrsi mstatus, 8");

    const _BsodReason* r = _bsod_find(tag);
    auto& d = ::M5.Display;
    d.setRotation(1);
    d.fillScreen(BSOD_BG);
    const int W = d.width();
    const int H = d.height();
    const int ML = 48;
    d.setFont(&fonts::Font8);
    d.setTextColor(BSOD_FG, BSOD_BG);
    d.setTextDatum(top_left);
    d.drawString(":(", ML, 28);
    d.setFont(&fonts::Font4);
    d.drawString("P4-Tab OS ran into a problem and needs to restart.", ML, 162);
    d.setTextColor(BSOD_ACCENT, BSOD_BG);
    d.drawString(r->headline, ML, 212);
    d.setFont(&fonts::Font2);
    d.setTextColor(BSOD_FG, BSOD_BG);
    int y = 268;
    for (int i = 0; i < 6 && r->detail[i]; i++, y += 26)
        d.drawString(r->detail[i], ML, y);
    y += 8;
    d.drawFastHLine(ML, y, W - ML * 2, BSOD_DIM);
    y += 18;
    d.setTextColor(BSOD_DIM, BSOD_BG);
    static char buf[160];
    snprintf(buf, sizeof(buf), "PC: 0x%08X   RA: 0x%08X   SP: 0x%08X",
             (unsigned)pc, (unsigned)ra, (unsigned)sp);
    d.drawString(buf, ML, y); y += 26;
    snprintf(buf, sizeof(buf), "REASON: %s   BAD ADDR (mtval): 0x%08X",
             tag ? tag : "unknown", (unsigned)badaddr);
    d.drawString(buf, ML, y); y += 26;
    uint32_t s = millis() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %02u:%02u:%02u   Free heap: %u KB   Core: %d",
             (unsigned)(s/3600), (unsigned)((s%3600)/60), (unsigned)(s%60),
             (unsigned)(esp_get_free_heap_size() / 1024), (int)xPortGetCoreID());
    d.drawString(buf, ML, y); y += 26;
    static const char* kRR[] = {
        "Unknown","Power-on","External pin","Software",
        "Panic/Guru","Interrupt WDT","Task WDT","Other WDT",
        "Deep sleep","Brownout","SDIO"
    };
    esp_reset_reason_t rr = esp_reset_reason();
    snprintf(buf, sizeof(buf), "Previous reset: %s",
             ((unsigned)rr < 11) ? kRR[(int)rr] : "Unknown");
    d.drawString(buf, ML, y);
    d.fillRect(0, H - 56, W, 56, BSOD_BAR);
    d.setTextColor(BSOD_FG, BSOD_BAR);
    d.setTextDatum(middle_center);
    d.drawString("Note the error above, then press the reset button to restart.", W/2, H-28);
}

#include "hal/wdt_hal.h"
#include "soc/timer_group_struct.h"

static void IRAM_ATTR __attribute__((noreturn))
_bsod_panic_handler(arduino_panic_info_t* info, void* ) {

    esp_rom_printf("BSOD: handler entered\n");

    {
        // stop the watchdog from resetting the board mid-draw so the crash
        // screen actually stays on screen long enough to read
        wdt_hal_context_t bsod_wdt0 = { .inst = WDT_MWDT0, .mwdt_dev = &TIMERG0 };
        wdt_hal_write_protect_disable(&bsod_wdt0);
        wdt_hal_disable(&bsod_wdt0);
        wdt_hal_write_protect_enable(&bsod_wdt0);
    }
    esp_rom_printf("BSOD: wdt0 disabled\n");

    uint32_t mtval = 0, sp = 0;
    __asm__ volatile (
        "csrr %0, mtval\n"
        "mv   %1, sp\n"
        : "=r"(mtval), "=r"(sp)
    );

    uint32_t pc = (uint32_t)(uintptr_t)info->pc;
    uint32_t ra = info->backtrace_len > 1 ? info->backtrace[1] : 0;

    const char* tag = info->reason;

    esp_rom_printf("BSOD: about to draw (reason=%s)\n", tag ? tag : "?");
    esp_rom_printf("BSOD: PC=0x%08x RA=0x%08x SP=0x%08x BAD_ADDR=0x%08x\n",
                   (unsigned)pc, (unsigned)ra, (unsigned)sp, (unsigned)mtval);
    if (info->backtrace_len > 0) {
        esp_rom_printf("BSOD: backtrace:");
        for (int _i = 0; _i < info->backtrace_len && _i < 8; _i++)
            esp_rom_printf(" 0x%08x", (unsigned)info->backtrace[_i]);
        esp_rom_printf("\n");
    }
    _bsod_draw(tag, pc, ra, sp, mtval);
    esp_rom_printf("BSOD: draw returned, entering halt loop\n");

    while (true) { __asm__ volatile("nop"); }
}

inline void bsod_install() {
    set_arduino_panic_handler(_bsod_panic_handler, nullptr);
}

