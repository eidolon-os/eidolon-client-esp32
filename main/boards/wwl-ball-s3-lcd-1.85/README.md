# WWL Ball S3 LCD 1.85

This is the first-generation WWL Ball S3 LCD 1.85 board.

## Hardware

- ESP32-S3 with 16 MB flash and octal PSRAM
- 360x360 ST77916 QSPI LCD
- No touch controller
- Raw I2S microphone: SCK GPIO15, WS GPIO2, DIN GPIO39
- Raw I2S speaker: BCLK GPIO48, LRCK GPIO38, DOUT GPIO47
- TCA9554 at `0x20` on I2C GPIO10/GPIO11
- BOOT GPIO0, power button GPIO6, power control GPIO7
- Backlight GPIO5

The first-generation board has no playback reference channel, so device-side AEC
and full-duplex barge-in are not enabled. Eidolon builds use PTT by default.

## Display and expression assets

Eidolon builds select the emote animation style
(`CONFIG_USE_EMOTE_MESSAGE_STYLE=y`): `InitializeDisplay()` constructs
`emote::EmoteDisplay` on the ST77916 panel instead of `SpiLcdDisplay`
(`SpiLcdDisplay` remains the `#else` path when the style is off).

At build time the WWL block in `main/CMakeLists.txt` sets
`EMOTE_RESOLUTION "360_360"` and points `EMOTE_EXTERNAL_PATH` at this board's
`assets/` directory. With `CONFIG_FLASH_EXPRESSION_ASSETS=y`, the
`esp_emote_assets` packer (`scripts/spiffs_assets/build_all.py` in the managed
component) reads the board-local `assets/360_360/config.json`, `emote.json` and
`layout.json`, pulls the `emoji_large` EAF animations and the
`font_puhui_common_20_4` text font from the managed component, and writes
`expression_assets.bin` into the `assets` partition of
`partitions/v2/16m_eidolon.csv` (0x740000 bytes). `wakenet_model` is `none`
because this board builds with wake word detection disabled, so no speech model
is packed.

The `emoji_small` collection renders the face at about 50% size in the upper
center of the panel (`x=95`, `y=-92` with the mirrored eye layout), while the board-local
layout keeps the toast label at y=90. The packed-font chrome uses board-specific
vertical offsets: `InitializeDisplay()` calls
`EmoteDisplay::SetChromeLayoutDeltas({128, -50, 148})`, which places the mode
label below the face, the READY/ENDED state label below the bottom caption, and
keeps the caption at its existing position. The top-left presence and top-right
exit chips are not affected.

At runtime the emote asset strategy replaces the LVGL one: the `assets`
partition is memory-mapped onto the display's emote handle and
`emote_load_assets` brings the animation scene up, after which
`EmoteDisplay::OnAssetsLoaded` applies the pending emotion and the Eidolon
voice-chrome overlays (mode, state, exit, caption, presence). Hub-driven
`SetEmotion` names map to entries in `assets/360_360/emote.json`; unknown names
fall back to `neutral`. Reflashing only the app partition keeps the installed
expression assets; they change only when the packer output is flashed again.

In Hub mode, press BOOT to start a conversation. While in a conversation, hold
BOOT to talk and release it to send. A long press outside a conversation opens
Wi-Fi setup.

POWER short press toggles the display between off and its previously saved
brightness. POWER long press starts the board's Xiaozhi-style power-off standby:
the backlight turns off and the external power latch on GPIO7 is driven low, then
GPIO7 is reasserted high about 5 seconds later so the POWER button can still wake
the board. A second long press while in standby drives GPIO7 high, restores the
saved brightness, and clears standby. This is the board's power-latch
standby/wake sequence; it is not ESP32 deep sleep and does not truly disconnect
battery power.
