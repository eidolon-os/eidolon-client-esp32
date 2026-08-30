#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ESP32-S31-Korvo-1 V1.1.
//
// Every pin below is copied from Espressif's own BSP for this board —
// esp-dev-kits/examples/esp32-s31-korvo/examples/common_components/esp32_s31_korvo
// (include/bsp/esp32_s31_korvo.h) — not inferred from the user guide, whose
// tables disagree with it in places (it lists GPIO61 as BOOT; the BSP uses it
// for the LCD's SPI clock). The BSP is used as a datasheet, not as a
// dependency: pulling it in would drag esp_codec_dev ^1.5.9, esp_lvgl_adapter
// and esp_video 2.2.0 against the versions the LiveKit stack pins here.

// 16 kHz, not the 24 kHz the ESP-BOX-3 boards use. The ES8389 driver picks its
// clock dividers from a table indexed by (MCLK ratio, sample rate), and 24 kHz
// appears there exactly once — at a ratio of 800, i.e. a 19.2 MHz MCLK — which
// no frame this firmware can produce will match, leaving the codec without
// valid dividers.
// 16 kHz has 22 entries and is what the AFE runs at anyway, so this also removes
// a resampling step rather than adding one.
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// The ES8389 can add a copy of the DAC output to its capture stream, giving the
// AFE a real echo reference. Espressif's board definition for this hardware
// (esp_boards/esp32_s31_korvo_1, adc_cfg.label) spells out the slot order:
// [FL, FR, RE, NA] — mic 1, mic 2, reference, unused. Enabling it widens the
// capture frame to four slots rather than substituting one, so the reference
// costs neither microphone.
#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_3
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_5   // ESP -> codec
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6   // codec -> ESP

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_7
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_0
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_1
#define AUDIO_CODEC_ES8389_ADDR  ES8389_CODEC_DEFAULT_ADDR

// Espressif's BSP for this board uses 30 dB. At 40 the mics saturate against
// the on-board speakers at test volume — 29% of samples clipped to full scale.
#define AUDIO_CODEC_INPUT_GAIN_DB 30.0f

// No external MCLK, matching Espressif's board definition for this hardware
// (esp_boards/esp32_s31_korvo_1: sys_cfg.no_mclk = true). This is not merely a
// clock-source preference: es8389_set_fs() only computes the codec's dividers
// when use_mclk is false, so asking the codec to take MCLK skips its sample
// configuration entirely.
#define AUDIO_CODEC_USE_MCLK     false

// This board has no dedicated BOOT button on a GPIO. Its four keys sit on one
// ADC ladder (GPIO42: 0.38 V / 0.82 V / 1.34 V / 1.87 V), which needs a driver
// this project does not have yet. Nothing here maps to a plain GPIO button;
// download mode is entered over USB (esptool --before usb_reset).
#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define BUTTON_ADC_GPIO         GPIO_NUM_42
#define LED_WS2812_GPIO         GPIO_NUM_37

// ESP32-S3-LCD-EV-Board-SUB3: 4.3" 800x480, 16-bit parallel RGB565, ST7262E43.
// A plain RGB TFT driver — no vendor init sequence over SPI is required, which
// is why LCD_CS/MOSI/SCK below stay unused.
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_PCLK_HZ            (26 * 1000 * 1000)
#define DISPLAY_HSYNC_PULSE_WIDTH  1
#define DISPLAY_HSYNC_BACK_PORCH   40
#define DISPLAY_HSYNC_FRONT_PORCH  20
#define DISPLAY_VSYNC_PULSE_WIDTH  1
#define DISPLAY_VSYNC_BACK_PORCH   10
#define DISPLAY_VSYNC_FRONT_PORCH  5

#define DISPLAY_LCD_PCLK  GPIO_NUM_40
#define DISPLAY_LCD_DE    GPIO_NUM_43
#define DISPLAY_LCD_HSYNC GPIO_NUM_44
#define DISPLAY_LCD_VSYNC GPIO_NUM_45

#define DISPLAY_LCD_DATA0  GPIO_NUM_8   // B3
#define DISPLAY_LCD_DATA1  GPIO_NUM_9   // B4
#define DISPLAY_LCD_DATA2  GPIO_NUM_10  // B5
#define DISPLAY_LCD_DATA3  GPIO_NUM_11  // B6
#define DISPLAY_LCD_DATA4  GPIO_NUM_12  // B7
#define DISPLAY_LCD_DATA5  GPIO_NUM_13  // G2
#define DISPLAY_LCD_DATA6  GPIO_NUM_14  // G3
#define DISPLAY_LCD_DATA7  GPIO_NUM_15  // G4
#define DISPLAY_LCD_DATA8  GPIO_NUM_16  // G5
#define DISPLAY_LCD_DATA9  GPIO_NUM_17  // G6
#define DISPLAY_LCD_DATA10 GPIO_NUM_18  // G7
#define DISPLAY_LCD_DATA11 GPIO_NUM_19  // R3
#define DISPLAY_LCD_DATA12 GPIO_NUM_33  // R4
#define DISPLAY_LCD_DATA13 GPIO_NUM_34  // R5
#define DISPLAY_LCD_DATA14 GPIO_NUM_35  // R6
#define DISPLAY_LCD_DATA15 GPIO_NUM_36  // R7

// The subboard has no backlight control line and no DISP enable: the panel is
// lit whenever it is powered.
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_LCD_DISP      GPIO_NUM_NC

// GT1151, on the shared I2C bus, with neither reset nor interrupt routed.
#define DISPLAY_TOUCH_I2C_SDA_PIN AUDIO_CODEC_I2C_SDA_PIN
#define DISPLAY_TOUCH_I2C_SCL_PIN AUDIO_CODEC_I2C_SCL_PIN
#define DISPLAY_TOUCH_RST_PIN     GPIO_NUM_NC
#define DISPLAY_TOUCH_INT_PIN     GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
