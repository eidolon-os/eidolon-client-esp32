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
