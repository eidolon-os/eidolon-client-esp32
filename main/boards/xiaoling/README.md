# Xiaoling

ESP32-S3，一代灵境球 1.85 硬件；引脚及 ST77916 初始化序列来自
`aigf-esp32/main/boards/wwl-ball-s3-lcd-1.85`，保留的参考许可见
[LICENSE.reference](LICENSE.reference)。不依赖原 SD 卡 image、旧云服务或旧动画资源。

## 硬件

| 功能 | 配置 |
| --- | --- |
| Flash / PSRAM | 16 MB / 8 MB Octal |
| 圆屏 | ST77916，360×360，QSPI 80 MHz；自动识别第二版面板 |
| QSPI | CLK 40、CS 21、D0 46、D1 45、D2 42、D3 41 |
| I2C / 屏幕复位 | SDA 11、SCL 10；TCA9554 @ 0x20，EXIO0/1 |
| 背光 | GPIO5，高电平有效 |
| 麦克风 | I2S1，SCK 15、WS 2、DIN 39，右槽，16 kHz |
| 扬声器 | I2S0，BCLK 48、LRCK 38、DOUT 47，双槽，24 kHz |
| 独立唤醒芯片 | UART0，TX 43、RX 44，9600 8N1 |
| 按键 / 电源 | BOOT 0、电源键 6、电源保持 7 |
| 电池 | GPIO8 / ADC1_CH7；没有充电检测线 |
| 日志与烧录 | 原生 USB Serial/JTAG；不能占用 UART0 |

## 架构和交互

- `WifiBoard` 负责配网、网络和身份；`NoAudioCodecSimplex` 负责裸 I2S。
  共用 PCM16 设备适配让 LiveKit 使用原有音量和 I2S32 转换，不在会话层判断板型。
- UART 只识别完整 `A1 11 22 33 44 DD` 唤醒帧，支持分包、连续帧、噪声恢复和
  2 秒去重。唤醒进入 `Application::WakeWordInvoke`，由应用任务在 ConfigReady
  时发起会话；会话中不切换或打断。不开启 WakeNet、microWakeWord 或设备 AEC。
- 半双工沿用共用会话策略：自动收音，播放时关闭上行采集，无 barge-in。
- 复用 Companion 界面；252×252 安全区域完整落在圆屏内，没有虚构触摸能力。
- BOOT 单击开启/结束会话；需要设置时进入配网，长按进入设置。
  电源键长按 2 秒断电。校准电压持续低于 3.0 V 时断电；3.15 V 以上取消低压累计。
- 不迁移旧项目的自学习唤醒词、音乐播放器、SD 卡动画及无后备电源 RTC。
  唤醒词本身保留独立芯片当前配置，ESP32 不训练或修改它。

## 构建和烧录

配置唯一来源是 `config.json`。ESP-IDF 固定 5.5.4，产物与 sdkconfig 隔离到
`build/eidolon/xiaoling`，不修改其它开发板的 sdkconfig。

```sh
./scripts/eidolon/eidolon-xiaoling.sh build
./scripts/eidolon/eidolon-xiaoling.sh flash
./scripts/eidolon/eidolon-xiaoling.sh logs 30
```

首次替换旧固件且分区布局不同时，需要显式擦除后完整烧录：

```sh
./scripts/eidolon/eidolon-xiaoling.sh erase-flash --yes
./scripts/eidolon/eidolon-xiaoling.sh flash
```

擦除会移除旧 Wi-Fi 和身份配置，需要重新配网。烧录沿用共用的芯片/容量/分区校验、
分区表回读、OTA 槽激活和启动指纹验证。多个 USB 设备同时连接时必须指定
`EIDOLON_PORT` 或 `EIDOLON_USB_SERIAL`。

## 验证

```sh
c++ -std=c++17 -Wall -Wextra -Werror tests/xiaoling_wake_frame_test.cc -o /tmp/xiaoling-wake-test
/tmp/xiaoling-wake-test
```

硬件验收还包括配网/准入、实际说出芯片唤醒词、麦克风收音、扬声器播放、播放时无回声上行、
会话退出后再次唤醒、圆屏边界和电源键。完整构建与启动验证不能替代这组端到端验证。

## GitHub 依赖访问

SDK 使用 `main/idf_component.yml` 中 `eidolon-os/client-sdk-esp32` 的固定提交，
不自动跟随 `vendor/client-sdk-esp32` 的工作分支。改动 SDK 后应发布并更新 pin，
不能把本地分支名称当作实际构建版本。

命令行 Git 不会自动继承 macOS 网络设置中的 HTTP 代理。如果系统代理位于
`127.0.0.1:7890`，可只为该次构建设置：

```sh
https_proxy=http://127.0.0.1:7890 ./scripts/eidolon/eidolon-xiaoling.sh build
```

代理地址必须与本机实际设置一致。切换 IDF target、依赖配置改变或锁文件被重建时，
组件管理器会重新解析依赖；它的 Git 缓存更新也可能再次访问远端，固定提交不等于完全离线。
