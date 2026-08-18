# Eidolon ESP32 客户端

Eidolon ESP32 Client 是 Eidolon 具身智能硬件的设备端固件。它把 ESP32-S3
设备的屏幕、麦克风、扬声器、按键、摄像头和传感器接入一个受信任的 Eidolon Host，
负责设备配网、身份、Hub 准入、实时语音传输和设备端 UI。

这是一个 Eidolon 产品项目。启用 Eidolon Hub 模式后，固件不使用小智云链路；
上游项目仍是本仓库重要的硬件与驱动基础，相关来源和许可见
[上游与许可证](#上游与许可证)。

## 核心能力

- 基于 Espressif protocomm 与 Security 2（SRP6a）的安全配网，支持 SoftAP 或
  BLE；未配网的 SoftAP 设备以 `eidolon-XXXX` 广播。
- 一台设备只信任一个 Host：配网时固定目标 Hub 身份和证书，不会信任局域网中任意
  响应的服务。
- 通过 mDNS 发现 Eidolon Hub，并通过证书固定的 HTTPS 完成准入；Enrollment 状态
  持久化且可幂等恢复。
- 独立的 LiveKit 控制会话和语音会话，支持麦克风采集、扬声器播放、转写和设备状态
  事件。
- 支持按键说话、半双工和全双工交互配置；全双工构建必须具备验证过的设备端 AEC
  参考信号。
- 针对不同硬件的 LVGL 界面、对话状态、状态反馈和动态形象。
- P-256 设备身份与签名请求。
- 在具备相应硬件的构建上支持 Guard、雷达和 Owner Presence 集成。
- 固定组件版本、带构建指纹的板级构建脚本。

完整的配网与设备准入模型见
[docs/hub-onboarding.md](docs/hub-onboarding.md)。

## Eidolon 硬件配置

以下硬件具备持续维护的 Eidolon 构建入口：

| 硬件配置 | 构建脚本 |
| --- | --- |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | `scripts/eidolon/eidolon-esp32-s3-touch-amoled-2.06.sh` |
| Espressif ESP-BOX-3 | `scripts/eidolon/eidolon-esp-box-3.sh` |
| ALIENTEK ATK-DNESP32S3 | `scripts/eidolon/eidolon-atk-dnesp32s3.sh` |
| M5Stack CoreS3 | `scripts/eidolon/eidolon-m5stack-core-s3.sh` |
| M5Stack CoreS3 + StackChan 机身 | `scripts/eidolon/eidolon-m5stack-stackchan.sh` |

仓库中仍保留上游的其他开发板适配，但上表是具有明确 Eidolon 配置和维护脚本的硬件
集合。

## 架构

```text
Eidolon Mobile / 配网控制端
             │  protocomm（SoftAP 或 BLE）
             ▼
        ESP32 设备身份
             │  Wi-Fi + mDNS + 证书固定 HTTPS
             ▼
         Eidolon Host
             │  已批准的 Provider Assignment
             ▼
      LiveKit 控制与语音房间
```

Wi-Fi 配置和 Owner 准入是两个独立事实。设备加入网络并不能证明它属于某个 Owner；
Enrollment 审批、Provider 分配和撤销始终由 Host 负责。

## 构建与烧录

### 环境要求

- ESP-IDF 5.5.4（板级脚本会自动查找标准位置中的 ESP-IDF）
- Python 和 ESP-IDF 工具链依赖
- 受支持的 ESP32-S3 硬件及 USB 连接

如果 ESP-IDF 安装在非标准位置，可把根目录写入 `scripts/eidolon/idf.path`，或设置
`EIDOLON_IDF_PATH`。

### 示例

选择与目标硬件对应的脚本：

```bash
./scripts/eidolon/eidolon-m5stack-stackchan.sh build
./scripts/eidolon/eidolon-m5stack-stackchan.sh flash
./scripts/eidolon/eidolon-m5stack-stackchan.sh monitor
```

不带参数运行脚本可进入交互菜单。脚本会选择开发板、分区表、交互模式和必要的
Eidolon 配置，建议优先使用板级脚本，而不是手工拼装通用 `idf.py` 配置。

## 首次设置

1. 烧录一个 Eidolon 硬件配置。
2. 让未完成 Commissioning 的设备进入配网模式。
3. SoftAP 构建选择 `eidolon-XXXX` 配网热点；BLE 构建通过 BLE 提供相同的配网契约。
4. 使用 Eidolon Mobile 或兼容控制端写入 Wi-Fi 凭据和目标 Host 的信任材料。
5. 设备加入 Wi-Fi，只发现已绑定的 Host，并创建或恢复 Enrollment。
6. Host 审批并完成 Provider Assignment 后，设备进入可用的语音/控制状态。

开发构建使用 Kconfig 中声明的开发配网 verifier。生产固件必须使用与制造身份绑定的
单设备配网凭据。

## 测试

协议和状态机测试可直接在开发机上构建，位于 `tests/`。例如：

```bash
./tests/run_device_provisioning_protocol_tests.sh
./tests/run_hub_onboarding_protocol_tests.sh
./tests/run_device_event_bus_tests.sh
```

发布板级固件前，还应完成一次完整固件构建，以及对应硬件的音频/AEC 验证。

## 仓库结构

- `main/eidolon/` — Eidolon 配网、身份、准入、LiveKit、控制、Presence 和 UI 实现。
- `main/boards/` — 开发板支持与硬件集成。
- `scripts/eidolon/` — 受支持的 Eidolon 构建、烧录与验证入口。
- `tests/` — 主机端协议与状态机测试。
- `docs/` — 产品契约和硬件验证说明。
- `partitions/v2/` — Eidolon 及继承的 ESP32 分区表。

## 上游与许可证

本仓库衍生自采用 MIT 许可证的
[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 项目，采用混合许可证。
上游和各贡献者拥有版权的部分继续适用 [MIT License](LICENSE)。

Li Jinsong 拥有版权的 Eidolon 原创内容适用
[PolyForm Noncommercial License 1.0.0](LICENSE.eidolon)。超出该许可证允许范围的
商业使用需要另行取得书面授权，请联系
[lijinsong@aimanthor.com](mailto:lijinsong@aimanthor.com)。

准确的适用范围和必要声明见 [LICENSING.md](LICENSING.md) 与 [NOTICE](NOTICE)。

## 参与贡献

欢迎提交 Issue 和范围明确的 Pull Request。请说明目标硬件、交互模式和已完成的验证。
配网、身份、准入或 Provider Binding 相关修改应包含协议/状态机测试，并保持当前的
fail-closed 信任模型。
