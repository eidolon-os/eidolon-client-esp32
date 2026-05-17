# Eidolon Hub mDNS 发现与配置拉取

本文档描述 ESP32 客户端在 WiFi 配网成功后，通过局域网 mDNS 发现 Eidolon Hub、拉取 LiveKit 初始化配置的设计与实现说明。对应 Hub 端 [`eidolon_hub/hub/core/discovery.py`](https://github.com/eidolon/eidolon_hub/blob/main/hub/core/discovery.py)。

## 阶段范围

| 阶段 | 内容 |
|------|------|
| **阶段一** | mDNS 发现 → `GET config_url` → NVS `eidolon`；不接小智云端 OTA 配置 |
| **阶段二（当前）** | LiveKit 进房与双向语音，见 [livekit-integration_zh.md](livekit-integration_zh.md) |
| **阶段 2b** | UI 手动进/离房、Hub `firmware` OTA |
| **长期** | 移除小智 MQTT/WebSocket 协议；固件/资源升级继续复用现有 `Ota` / `Assets` |

## 激活流程（Eidolon）

```text
WiFi Connected
  → kDeviceStateActivating
  → CheckAssetsVersion()          # 保留
  → HubActivator::Run()           # mDNS + HTTP + NVS
  → [成功] MAIN_EVENT_ACTIVATION_DONE → Idle →（若启用自动进房）LiveKit InRoom
  → [失败] 保持 Activating，重试/Alert，不播成功音
```

不调用：`CheckNewVersion()`（小智云端）、`InitializeProtocol()`（小智协议）。

## Hub mDNS 约定

| 项 | 值 |
|----|-----|
| Service type | `_eidolon-hub._tcp` |
| Service name | `Eidolon Hub._eidolon-hub._tcp` |
| Hostname | `eidolon-hub.local` |
| TXT `txtvers` | `1` |
| TXT `api` | `v1` |
| TXT `version` | Hub 软件版本（可选） |
| TXT `config_url` | `http://{LAN_IP}:{port}/api/config` |

TXT 扩展：未知键保留在 `mdns_txt_json`；`txtvers=1` 必填 `api`、`config_url`。

## HTTP 配置

```http
GET {config_url}
X-Device-ID: {MAC，形如 aa:bb:cc:dd:ee:ff}
```

响应（节选）：

```json
{
  "success": true,
  "config": {
    "server_url": "ws://192.168.x.x:7880",
    "token": "<jwt>",
    "identity": "device-id",
    "room_name": "esp32-xxxx",
    "audio": { "sample_rate": 16000, "channels": 1 }
  }
}
```

固件（阶段二，同 URL 可选返回）：

```json
"firmware": { "version": "1.2.0", "url": "http://...", "force": 0 }
```

版本比较在 **HTTP 层**完成（POST 设备信息 + 响应 `firmware` + 设备端 `IsNewVersionAvailable`），**不在 mDNS 层**。

## NVS（命名空间 `eidolon`）

| Key | 说明 |
|-----|------|
| `config_url` | 最近一次 mDNS 的 config URL |
| `mdns_txt_json` | 完整 TXT 快照 |
| `txtvers` | TXT schema 版本 |
| `server_url` / `token` / `identity` / `room_name` | LiveKit |
| `sample_rate` / `channels` | 音频参数 |
| `hub_version` / `hub_api` | TXT 冗余字段 |

## 代码结构（`main/eidolon/`）

| 模块 | 职责 |
|------|------|
| `hub_types.h` | 类型与 TXT 键名常量 |
| `hub_txt_parser` | TXT 解析与 `txtvers` 校验 |
| `hub_discovery` | `mdns_query_ptr` |
| `hub_config_client` | HTTP GET + JSON |
| `hub_config_store` | NVS 读写 |
| `hub_activator` | 重试与编排 |

## 构建配置

- **Component Registry**：ESP-IDF 5.x 使用 `espressif/mdns`（`main/idf_component.yml`）。
- **CMake**：`CONFIG_EIDOLON_HUB_MODE` 时 `PRIV_REQUIRES espressif__mdns`。
- **Kconfig**：`main/Kconfig.projbuild` → menu「Eidolon Hub」；2.06 板默认 `EIDOLON_HUB_MODE=y`。
- **sdkconfig**：Eidolon 脚本写入 `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y` 等。

## 测试

1. 启动 Hub：`deploy/dev/run_all.sh`
2. 板子配网后串口：mDNS 命中、`config_url`、HTTP 200、NVS 写入
3. 负例：Hub 未开 → 重试/Alert，不进 Idle
4. `idf.py monitor` 下确认无小智 MQTT/WS 连接（阶段一预期）

## 相关文档

- [LiveKit 集成（阶段二）](livekit-integration_zh.md)
- [BluFi 配网](blufi_zh.md)
- Hub 部署：[eidolon_hub/deploy/dev/README.md](https://github.com/eidolon/eidolon_hub/blob/main/deploy/dev/README.md)
