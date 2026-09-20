# Companion UI render and interaction checks

Run `bash tests/run_companion_ui_tests.sh` from the ESP32 repository. This uses the
resolved `managed_components` LVGL and actual BOX-3/StackChan fonts. CMake, a host
C/C++ compiler and the existing managed dependencies are required. No display
server, device, service or network connection is used.

Only the ESP clock and display lock are replaced. The production view, geometric
skin, output gate and LVGL widgets/rendering are compiled unchanged. Assertions
exercise the input/output matrix, layout bounds, text suppression, notification
expiry without replacing dialogue, caption coalescing/paging/expiry, immediate
microphone status, supported icon glyphs, details scrolling and PTT press/release
across model updates. Chinese coverage uses the actual production Noto cbin
resource with a host-only 32-bit pointer-layout adapter; glyphs, bitmaps and
metrics are unchanged. It reproduces `我叫小何。` with the basic subset and checks
resource-font rebinding plus whole-line pagination at the resource's line height.
BOX-3 uses the 16px Noto resource. Incremental Chinese and English snapshots
check the full 304px strip, left alignment and following the newest spoken line.

The executable writes PPM snapshots to the build directory's `screenshots`
folder. By default this is `$TMPDIR/eidolon-companion-ui-tests/screenshots`
(`/tmp` when TMPDIR is unset). Set `EIDOLON_UI_TEST_BUILD` to override it.
Network and battery values in snapshots are synthetic test inputs; the images
prove renderer output, not physical touch calibration or panel color accuracy.
