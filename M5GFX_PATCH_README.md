# Required M5GFX patch — expose the DSI panel handle

The real fix for the tearing (see gfx_driver.cpp comments) needs direct access
to the ESP-IDF DPI panel handle that M5GFX's `Panel_DSI` already creates and
holds privately, so gfx_driver.cpp can drive the panel's real (but unused)
second hardware framebuffer.

This can't be done from the sketch alone — it needs one small public accessor
added to M5GFX itself. Find your installed copy of M5GFX (Arduino IDE:
`~/Documents/Arduino/libraries/M5GFX/`, or wherever your board manager /
library manager put it) and apply this:

## File: src/lgfx/v1/platforms/esp32p4/Panel_DSI.hpp

Add this public method inside `struct Panel_DSI`, next to `getBusDSI()`:

```cpp
    // Exposes the underlying ESP-IDF DPI panel handle so callers can drive
    // its real double-buffering (num_fbs=2) directly — M5GFX itself only
    // ever uses frame buffer 0 (see init()), leaving the second buffer idle
    // and giving no tear protection. gfx_driver.cpp uses this to implement
    // an actual flip instead of writing straight into the live-scanned
    // buffer.
    esp_lcd_panel_handle_t getDsiPanelHandle(void) const { return _disp_panel_handle; }
```

That's the only change needed — `_disp_panel_handle` already exists as a
protected member, this just adds a public getter for it. No changes to
Panel_DSI.cpp are required.

## Why this can't be avoided

- `Panel_FrameBufferBase` (which `Panel_DSI` inherits from) implements
  `waitDisplay()` / `displayBusy()` as no-ops that always report "not busy" —
  there is no built-in vsync/tear signal exposed through the normal M5GFX
  drawing API for this panel type.
- `Panel_DSI::init()` calls
  `esp_lcd_dpi_panel_get_frame_buffer(_disp_panel_handle, 1, &(_config_detail.buffer))`
  — the `1` there means "give me 1 buffer, i.e. index 0", not "buffer #1".
  Buffer index 1 (the second one ESP-IDF already allocated because M5GFX
  configured `num_fbs = 2`) is never touched by M5GFX at all.
- The IDF driver (`components/esp_lcd/dsi/esp_lcd_panel_dpi.c`) DOES support a
  real flip: `esp_lcd_panel_draw_bitmap()` checks whether the pointer you
  pass it is one of its own frame buffers; if so it skips the copy, does a
  cache write-back, and switches `cur_fb_index` — and that switch is only
  picked up at the next DMA restart (i.e. the next frame boundary,
  effectively vsync), not mid-scan. That's the only tear-free path this
  hardware/driver combination actually offers, and it's what gfx_driver.cpp
  now uses.
