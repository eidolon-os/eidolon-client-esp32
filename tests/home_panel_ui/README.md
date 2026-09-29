# Smart home panel render and interaction checks

Run `bash tests/run_home_panel_ui_tests.sh` from the ESP32 repository. It builds
the korvo-1 `HomePanelView` with the resolved `managed_components` LVGL, the
production `UiStateProjector`, the smart home store and wire, and the panel's
generated fonts, and drives them with the SDK goldens in
`../eidolon_sdk/contracts/smarthome/v1/golden`. Only the ESP clock, logging,
RNG and display lock are replaced. CMake and a host C/C++ compiler are
required; no device, service or network is used.

Text is drawn with the assets partition's `font_puhui_common_20_4` cbin (through
the `tests/companion_ui` host fixture), as the panel runs once assets are
loaded; the system pages are also drawn with the builtin 206-character subset
the panel boots with.

Assertions cover every visible character drawing in its label's font chain,
Chinese system copy for every projected scene (and korvo-1's SET key, not
BOOT), tiles and the result card staying inside their areas, the panel opening
on its own area until one is picked, and touch still producing the same
execute, scene and sync requests and UI intents (none while the cache is
stale).

One PPM per state is written to the build directory's `screenshots` folder,
by default `$TMPDIR/eidolon-home-panel-ui-tests/screenshots`
(`EIDOLON_UI_TEST_BUILD` overrides the build directory). The images prove
renderer output, not panel color accuracy: the glass is RGB565.

Fonts are regenerated with `scripts/eidolon/gen_home_panel_fonts.py` after an
icon is added to `main/eidolon/views/home_panel_icons.h` or copy is added to
`main/eidolon/views/home_panel_copy.cc`.
