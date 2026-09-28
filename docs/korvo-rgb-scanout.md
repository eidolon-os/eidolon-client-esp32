# Korvo-1 RGB scanout investigation (2026-09-28)

## Observations and evidence limits

The physical ESP32-S31-Korvo-1 shows a vertically wrapped image across tabs,
and the user reports flickering pixels. Reset previously restored the image but
the fault returned. This is not evidence that reset fixed its cause.

The pre-change 30-second serial sample booted firmware `5f8a09546+dirty`,
connected LiveKit, and received the 18-device home snapshot. It contained no
`LCD underrun` or panic. Opening this CP2102N serial port can reset the board,
even with the capture script's `noreset` argument. Consequently the sample did
not preserve the original fault or establish which operation triggered it.

## Established configuration defect

`Korvo1Board::InitializeRgbDisplay` used two PSRAM framebuffers and two internal
bounce buffers, each holding 10 lines. In ESP-IDF 6.1's
`components/esp_lcd/rgb/esp_lcd_panel_rgb.c`, `lcd_rgb_panel_eof_handler` advances
a software cursor and `lcd_rgb_panel_fill_bounce_buffer` copies the next pixels
on each EOF interrupt. Scanout depends on these CPU refills meeting their
deadline. Delayed/missed refills can transmit stale data and lose the alignment
between that cursor and the continuously scanning panel.

The same driver's `lcd_rgb_panel_try_restart_transmission` repairs this kind of
desynchronization only under `RGB_LCD_NEEDS_SEPARATE_RESTART_LINK`, defined for
ESP32-S3, not ESP32-S31. `esp_lcd_rgb_panel_restart` is unsupported on S31.

Earlier changes enabled (`d791b4f6`) and then disabled (`8557669e`)
`LCD_RGB_ISR_IRAM_SAFE`. Disabling avoids executing the PSRAM-copy ISR while
cache is unavailable, but leaves the interrupt-driven pixel supply vulnerable
to stalls. Enabling it again does not make those PSRAM accesses safe. This
explains why the earlier correction was incomplete. The exact stall causing
the photographed incident remains unmeasured; flash writes are a possible
trigger, not an observed fact. Pixel flicker is compatible with late refills,
but needs physical verification and must not be declared independently proven.

## Correction and synchronization contract

Use S31 AXI DMA to read PSRAM framebuffers directly (`bounce_buffer_size_px=0`).
This matches Espressif's `esp-claw` board definition at
`application/edge_agent/boards/espressif/esp32_s31_korvo1/board_devices.yaml`.
Keep the board's 18 MHz pixel clock, documented panel timings, 64-byte bursts,
and our two framebuffers for tear avoidance. Do not add periodic resets,
coordinate offsets, or a custom display driver.

Keep the existing LVGL synchronization:

1. LVGL draws into a full-size framebuffer.
2. `esp_lvgl_port` submits the whole frame on the last flush.
3. The IDF driver writes back its cache and requests an AXI DMA link switch.
4. LVGL waits for `on_frame_buf_complete` before reusing the old framebuffer.

The port's historical `bb_mode=true` name is misleading on IDF 6: it selects
`on_frame_buf_complete` even without bounce buffers. It must remain enabled;
plain VSYNC does not guarantee that DMA has released the old buffer. Other
boards' scanout configurations and rendering behavior are unchanged.

## Verification

Build and flash with `scripts/eidolon/eidolon-korvo-1.sh`. Boot must report
`RGB scanout: direct PSRAM DMA, 2 framebuffers, pclk=18000000 Hz`, reach the
home panel, and continue receiving snapshots without panic or LCD underrun.

Physical acceptance is still required: observe idle pixels, switch tabs and
rooms repeatedly, enter/exit voice sessions, issue device commands, and observe
the screen while results arrive. Include a cold boot and several minutes of
normal use. Both cyclic wrapping and flickering must stay absent. A clean boot
log alone cannot establish visual acceptance. If either persists, preserve the
fault and collect its timing before resetting; investigate bandwidth/electrical
causes from evidence instead of reintroducing bounce buffers or changing panel
timings speculatively.
