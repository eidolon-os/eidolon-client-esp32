# Companion UI render and interaction checks

Run `bash tests/run_companion_ui_tests.sh` from the ESP32 repository. This uses the
resolved `managed_components` LVGL and actual BOX-3/StackChan fonts. CMake, a host
C/C++ compiler and the existing managed dependencies are required. No display
server, device, service or network connection is used.

Only the ESP clock and display lock are replaced. The production view, geometric
skin, output gate and LVGL widgets/rendering are compiled unchanged. Assertions
exercise the input/output matrix, layout bounds, text suppression, notification
expiry, details scrolling and PTT press/release across model updates.

The executable writes eight PPM snapshots to the build directory's `screenshots`
folder. By default this is `$TMPDIR/eidolon-companion-ui-tests/screenshots`
(`/tmp` when TMPDIR is unset). Set `EIDOLON_UI_TEST_BUILD` to override it.
Network and battery values in snapshots are synthetic test inputs; the images
prove renderer output, not physical touch calibration or panel color accuracy.
