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
