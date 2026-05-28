# Waveshare 1.51 Inch Transparent OLED Aux Display Plan

## Summary

Add a 1.51 inch transparent OLED auxiliary display to the existing Waveshare
ESP32-S3-Touch-AMOLED-2.06 device. The first version should treat the aux
display as an "emotion/status core": eyes, breathing animation, connection
state, listening/thinking/speaking state. It should not replace the main AMOLED
screen and should not display chat transcripts.

Target hardware: Waveshare 1.51inch Transparent OLED, treated as an SSD1309
128x64 monochrome display. Use I2C by default.

## Key Changes

- Hardware wiring:
  - `VCC -> 3.3V`
  - `GND -> GND`
  - `SDA -> GPIO15`
  - `SCL -> GPIO14`
  - Share the existing I2C bus used by the 2.06 board.
  - Default OLED I2C address: `0x3C`.
  - Add a Kconfig option for `0x3C` / `0x3D` if needed.

- Add a lightweight aux display driver:
  - Create an `AuxTransparentOled` component/class.
  - Do not reuse the existing `OledDisplay` class.
  - Do not register the aux screen as a second LVGL display.
  - Use the existing `i2c_master_bus_handle_t` to create an OLED I2C device.
  - Send SSD1309 init commands, maintain a 128x64 framebuffer, and refresh by
    page.
  - Limit animation refresh to roughly 10-15 FPS.
  - Allow idle/connection breathing animations to run slower, around 2-5 FPS.

- Initial visual states:
  - `Idle/Ready`: centered eyes plus slow breathing border.
  - `Connecting/Reconnecting`: small scanline or dot-matrix loop.
  - `Listening/UserSpeaking`: open eyes plus subtle bottom waveform.
  - `AgentThinking`: scanning dots or short pulse animation.
  - `AgentSpeaking`: mouth/waveform animation.
  - `Error/Config`: simple warning icon, no long text.

- State integration:
  - Extend or map the existing `EidolonUiSnapshot` state into aux display
    states.
  - After `EidolonUiPresenter::ApplySnapshot()`, call an aux display hook such
    as `eidolon::UpdateAuxDisplay(snapshot)`.
  - Instantiate the aux display in the Waveshare
    `esp32-s3-touch-amoled-2.06` board implementation and register the updater
    there.
  - Keep the main `CustomLcdDisplay` responsible only for the main AMOLED UI
    and voice session button.

- Configuration:
  - Add `CONFIG_AUX_TRANSPARENT_OLED`.
  - Scope it to `BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_2_06`.
  - Add `CONFIG_AUX_TRANSPARENT_OLED_I2C_ADDR`, defaulting to `0x3C`.
  - If aux display initialization fails, log a warning and allow the main
    device to continue booting.

## Test Plan

- Build:
  - `idf.py set-target esp32s3`
  - Build the `esp32-s3-touch-amoled-2.06` target with aux display disabled.
  - Build the same target with aux display enabled.

- Hardware smoke test:
  - On boot, the aux OLED shows idle eyes.
  - The main AMOLED keeps the existing UI behavior.
  - LiveKit connect, disconnect, and reconnect states update the aux OLED.
  - Listening, user speaking, thinking, and assistant speaking states update
    the aux OLED animation.

- Stability:
  - Run a 10 minute continuous voice session.
  - Confirm there is no I2C error flood.
  - Confirm the main display does not visibly stutter.
  - Confirm LiveKit audio quality does not noticeably regress.
  - Boot with the aux display disconnected or at the wrong I2C address and
    confirm the main device still starts normally.

## Assumptions

- The first prototype uses the Waveshare bare transparent OLED module.
- The first version focuses on emotion/status visuals, not transcript text.
- The aux display uses I2C, not SPI, to keep wiring simple and avoid sharing
  the main AMOLED QSPI/SPI resources.
- The target display is compatible with SSD1309-style 128x64 monochrome OLED
  initialization and page refresh.
