# VSYNC Notes

## Fastest Flicker-Free Setup

The first setup that was both fast and visually stable was:

- LVGL `LV_DISPLAY_RENDER_MODE_DIRECT`
- two full RGB framebuffers
- VSYNC-gated framebuffer swap on the last LVGL flush of a frame
- 10-line RGB bounce buffer

In code this means:

- `Arduino_ESP32RGBPanel(..., bounce_buffer_size_px = 480 * 10, num_fbs = 2)`
- `lv_display_set_buffers(disp, framebufferA, framebufferB, FRAMEBUFFER_SIZE, LV_DISPLAY_RENDER_MODE_DIRECT)`
- in `flush_cb`, only swap when `lv_display_flush_is_last(disp)` is true
- wait for panel VSYNC before calling `rgbpanel->drawBitmap(...)`

## Why This Worked

Earlier direct-mode attempts rendered quickly but visibly flickered while scrolling.

The missing piece was synchronizing the full-frame swap to VSYNC instead of exposing the new framebuffer immediately.

The bounce buffer also mattered. Without it, display behavior was worse.

## What We Tried First

These improved things but were not the final answer:

- partial mode + manual `gfx->flush()` once per loop
- small RGB bounce buffer in partial mode
- flatter denser list styling

These either did not help enough or made rendering worse:

- direct mode without proper VSYNC swap
- forcing lower PCLK
- disabling Wi-Fi sleep
- simple porch retuning

## Practical Conclusion

If this display stack needs both speed and visual stability, the best path found here is:

1. direct mode
2. double framebuffer
3. VSYNC-synchronized swap
4. small bounce buffer
