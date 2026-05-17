# Eidolon Hub microWakeWord 唤醒词

Hub 固件使用独立的 **microWakeWord** 通路（TFLite），不经过小智 `audio_service` 与乐鑫 WakeNet / ESP-SR。

- 方案：[micro-wake-word-plan_zh.md](micro-wake-word-plan_zh.md)
- 实现：[micro-wake-word-implementation_zh.md](micro-wake-word-implementation_zh.md)

## 默认唤醒词

- 预训练模型：**hey jarvis**（英文）
- 模型来源：[esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models)（Apache-2.0）
- 推理实现参考：[ESPHome micro_wake_word](https://github.com/esphome/esphome/tree/dev/esphome/components/micro_wake_word)

激活完成并进入 **ConfigReady** 后，对着设备说 **「hey jarvis」** 即可触发进房（与触屏、BOOT 短按并列）。

## 串口日志

| TAG | 含义 |
|-----|------|
| `EidolonAudioIn` | 共享 `BoxAudioCodec` 平台初始化 |
| `EidolonWakeWord` | 采麦输入任务、启用/暂停检测 |
| `EidolonMww` / `EidolonMwwDet` | TFLite 推理与检测结果 |

## 调参（menuconfig → Eidolon Hub）

- **EIDOLON_WAKE_WORD_ENABLE**：开关 microWakeWord
- **EIDOLON_WAKE_WORD_THRESHOLD**：概率阈值 0–255（默认 128 ≈ 0.5，与 `hey_jarvis.json` 一致）
- **EIDOLON_WAKE_WORD_COOLDOWN_MS**：两次触发最小间隔（默认 2000 ms）

进房（Connecting / InRoom）时自动 **暂停** 唤醒；离房回到 ConfigReady 后 **恢复**。

Eidolon 构建使用 `partitions/v2/16m_eidolon.csv`（OTA 分区略大于默认 `16m.csv`，以容纳 TFLite 与 microfrontend）。

## 验收

1. 激活后待机：串口出现 `audio platform ready`，`Enabling wake word detection`
2. 说 "hey jarvis"：日志 `hey_jarvis detected` → Connecting → InRoom
3. 通话中再说唤醒词：不应重复进房
4. 结束对话回 ConfigReady：唤醒恢复，可再次触发
5. 触屏进房：仍正常

## 替换模型

1. 用 [OHF-Voice/micro-wake-word](https://github.com/OHF-Voice/micro-wake-word) 训练或下载 `.tflite`
2. 替换 `main/eidolon/wake_word/models/hey_jarvis.tflite` 并重新编译
3. 按模型 JSON 调整 `THRESHOLD` 与 `sliding_window_average_size`（当前代码窗口固定为 10）
