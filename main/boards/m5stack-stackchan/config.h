#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// M5Stack StackChan configuration (CoreS3 base + servo body)

#include <driver/gpio.h>
#include <driver/uart.h>

// StackChan captures a SINGLE mic with NO device-side AEC: its ES7210 has no
// validated clean playback-reference channel (a 2-channel AEC there cancels the
// near-end voice, not just echo). StackChan is not full-duplex — echo is avoided
// by closing the mic during playback (half_duplex / ptt), so no reference channel
// is needed. (Full-duplex boards like esp-box-3 keep this true with a validated
// reference.) Pairs with USE_DEVICE_AEC=n for this board.
#define AUDIO_INPUT_REFERENCE    false
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_0
#define AUDIO_I2S_GPIO_WS GPIO_NUM_33
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_34
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_14
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13

#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_12
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_11
#define AUDIO_CODEC_AW88298_ADDR AW88298_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define DISPLAY_SDA_PIN GPIO_NUM_NC
#define DISPLAY_SCL_PIN GPIO_NUM_NC
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true



/* Camera pins */
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK  GPIO_NUM_NC // 像素时钟 (固定由 20MHz 外部晶振输入) 
#define CAMERA_PIN_SIOD GPIO_NUM_NC  // 串行时钟 Using existing I2C port
#define CAMERA_PIN_SIOC GPIO_NUM_NC  // 串行时钟 Using existing I2C port
#define CAMERA_PIN_D0 GPIO_NUM_39
#define CAMERA_PIN_D1 GPIO_NUM_40
#define CAMERA_PIN_D2 GPIO_NUM_41
#define CAMERA_PIN_D3 GPIO_NUM_42
#define CAMERA_PIN_D4 GPIO_NUM_15
#define CAMERA_PIN_D5 GPIO_NUM_16
#define CAMERA_PIN_D6 GPIO_NUM_48
#define CAMERA_PIN_D7 GPIO_NUM_47
#define CAMERA_PIN_VSYNC GPIO_NUM_46
#define CAMERA_PIN_HREF GPIO_NUM_38
#define CAMERA_PIN_PCLK GPIO_NUM_45

#define XCLK_FREQ_HZ 20000000


/* -------- StackChan body: Feetech SCSCL serial-bus servos + PY32 IO-expander -------- */
/* Facts replicated from the factory StackChan firmware (hal_servo.cpp / hal_io_expander.cpp). */

// Servo bus UART: 2-wire full-duplex, no direction pin.
#define SERVO_UART_PORT      UART_NUM_1
#define SERVO_UART_TX_PIN    GPIO_NUM_6
#define SERVO_UART_RX_PIN    GPIO_NUM_7
#define SERVO_UART_BAUD      1000000

// Servo IDs on the bus.
#define SERVO_ID_YAW         1     // horizontal / pan
#define SERVO_ID_PITCH       2     // vertical / tilt

// Home position in raw SCSCL units (0..1000). angle 0 maps to these zero positions.
#define SERVO_YAW_HOME_RAW   460
#define SERVO_PITCH_HOME_RAW 620

// raw = zero_raw + deg * SERVO_RAW_PER_DEG  (1 raw step = 0.3125 deg -> 3.2 raw/deg).
#define SERVO_RAW_PER_DEG    3.2f

// Mechanical angle limits (degrees). Pitch avoids its mechanical extremes.
#define SERVO_YAW_MIN_DEG    (-128.0f)
#define SERVO_YAW_MAX_DEG    ( 128.0f)
#define SERVO_PITCH_MIN_DEG  (   3.0f)
#define SERVO_PITCH_MAX_DEG  (  87.0f)

// Final raw safety clamp (hardware is 0..1023; factory clamps 0..1000).
#define SERVO_RAW_MIN        0
#define SERVO_RAW_MAX        1000

// PY32 IO-expander: gates servo power rail (pin 0 "VM EN") + drives 12 RGB (pin 13).
#define PY32_IOE_I2C_ADDR    0x6F
#define PY32_SERVO_POWER_PIN 0
#define PY32_RGB_DATA_PIN    13
#define PY32_RGB_LED_COUNT   12

#endif // _BOARD_CONFIG_H_
