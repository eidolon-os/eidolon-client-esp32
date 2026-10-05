#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/spi_master.h>

// 本硬件（灵境球 1.85 一代）没有 ES8311/ES7210 codec，麦克风与喇叭都是裸 I2S：
//   麦克风走 I2S_NUM_1，喇叭走 I2S_NUM_0，没有 MCLK。
// 以上引脚由 JTAG 读取出厂固件的 GPIO 矩阵实测得到。
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 单麦裸 I2S，没有回采参考通道，不能开设备侧 AEC
#define AUDIO_INPUT_REFERENCE    false

// 麦克风 -> I2S_NUM_1
#define AUDIO_I2S_MIC_GPIO_SCK   GPIO_NUM_15
#define AUDIO_I2S_MIC_GPIO_WS    GPIO_NUM_2
#define AUDIO_I2S_MIC_GPIO_DIN   GPIO_NUM_39
// 喇叭 -> I2S_NUM_0
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_48
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_38
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_47

// I2C 总线（TCA9554 IO 扩展 @0x20、RTC @0x51 都挂在这条上）
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_11
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_10

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define PWR_BUTTON_GPIO         GPIO_NUM_6
#define PWR_Control_PIN         GPIO_NUM_7
#define Voice_Control_PIN       GPIO_NUM_1

#define I2C_SCL_IO          GPIO_NUM_10       
#define I2C_SDA_IO          GPIO_NUM_11        

#define I2C_ADDRESS         ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000

#define DISPLAY_WIDTH       360
#define DISPLAY_HEIGHT      360
#define DISPLAY_MIRROR_X    false
#define DISPLAY_MIRROR_Y    false
#define DISPLAY_SWAP_XY     false

#define QSPI_LCD_H_RES           (360)
#define QSPI_LCD_V_RES           (360)
#define QSPI_LCD_BIT_PER_PIXEL   (16)

#define QSPI_LCD_HOST           SPI2_HOST
#define QSPI_PIN_NUM_LCD_PCLK   GPIO_NUM_40
#define QSPI_PIN_NUM_LCD_CS     GPIO_NUM_21
#define QSPI_PIN_NUM_LCD_DATA0  GPIO_NUM_46
#define QSPI_PIN_NUM_LCD_DATA1  GPIO_NUM_45
#define QSPI_PIN_NUM_LCD_DATA2  GPIO_NUM_42
#define QSPI_PIN_NUM_LCD_DATA3  GPIO_NUM_41
#define QSPI_PIN_NUM_LCD_RST    GPIO_NUM_NC
#define QSPI_PIN_NUM_LCD_BL     GPIO_NUM_5

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

// 本板无触摸屏。GPIO1/GPIO3 上曾预留过第二条 I2C 总线，实测总线上无任何器件。

#define DISPLAY_BACKLIGHT_PIN           QSPI_PIN_NUM_LCD_BL
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// UART Configuration
#define UART_PORT_NUM      UART_NUM_0
#define UART_BAUD_RATE     (9600)
#define UART_TXD_PIN       GPIO_NUM_43//GPIO_NUM_12//GPIO_NUM_43
#define UART_RXD_PIN       GPIO_NUM_44//GPIO_NUM_13//GPIO_NUM_44
#define UART_RTS_PIN       (-1)
#define UART_CTS_PIN       (-1)
#define UART_BUF_SIZE      (128)

//battery
#define BATTERY_PIN_NUM         GPIO_NUM_8   // ADC1_CH7
// 本板没有充电检测信号。GPIO4 实测悬空未连接（内部上拉/下拉翻转法验证，
// 电平完全跟随内部上下拉；同法测 GPIO0/GPIO6 按键则恒定不变），
// 出厂固件也从未把它配置成输入。充电状态改由电压回升判断，见 CheckBattery()。
#define BATTERY_CHARGE_DET_PIN  GPIO_NUM_NC

// RTC: PCF85063 兼容芯片，挂在 I2C0 (SCL=10/SDA=11)。
// 出厂固件从未初始化它（Control_1=0x80 处于 EXT_TEST，振荡器停摆）。
// ⚠ 实测该芯片无后备供电，会跟随主电源一起掉电，掉电后时间丢失。
//   详见 InitializeRtc() 的说明。驱动为硬件修版预留。
#define RTC_PCF85063_ADDR       0x51
// 低电截止关机电压，以及取消关机倒计时的回升电压（带迟滞）
#define BATTERY_SHUTDOWN_VOLTAGE 3.00f
#define BATTERY_RECOVER_VOLTAGE  3.15f

#define TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }


#endif // _BOARD_CONFIG_H_
