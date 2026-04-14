# First Line Issue

## Current state

The major idle flicker problem is effectively solved with:

- `Arduino_RGB_Display(..., auto_flush = false, ...)`
- `gfx->flush()` once per main loop
- RGB bounce buffer enabled with `TFT_BOUNCE_BUFFER_PX = 480 * 10`

This combination appears stable and keeps scrolling behavior acceptable.

## Remaining issue

There is still a very small rendering artifact on the first visible line at the top of the display.

- It is minor and easy to miss.
- It does not appear to affect the rest of the frame.
- It remained after small vertical timing tweaks.

## What was tried

These helped:

- manual framebuffer flush instead of `auto_flush`
- 10-line bounce buffer

These did not materially help:

- disabling Wi-Fi sleep
- forcing lower RGB PCLK
- `LV_DEF_REFR_PERIOD` from `33` to `16`

These changed behavior but were not acceptable:

- direct framebuffer rendering: made scrolling/rendering much worse
- 20-line bounce buffer: removed flicker but wrapped a visible band from top to bottom

## Timing experiments tried

Base vertical timing was:

- `vsync_polarity = 1`
- `vsync_front_porch = 10`
- `vsync_pulse_width = 8`
- `vsync_back_porch = 20`

Tried variations:

- `10 / 9 / 20` for front/pulse/back: no meaningful improvement
- `11 / 8 / 20` for front/pulse/back: no meaningful improvement

## Most likely next steps

If resuming later, try these in small isolated steps:

1. Test other small vertical timing adjustments around the current values.
2. Check whether the top-line artifact tracks bounce buffer size.
3. Investigate whether a framework build with `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` allows a larger bounce buffer without wrap.
4. If needed, inspect whether the panel wants slightly different porch values than the current ST7701 setup.
