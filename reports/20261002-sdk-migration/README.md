# SDK 必要性复核后的实际迁移与验收（2026-10-02）

本轮完成撤销 `f7b4279` 公共权限字段扩展的生产依赖，替换应用刷新机制，并同步核对官方最新。此记录接续 `../20261002-sdk-reassessment/README.md`，不覆盖以前的故障记录。

## 最终代码状态

- 主项目在 `main`，生产改动提交为 `3835ca3c57ac92ded07fa3ff00a1a7bd1b64fe3b`。
- SDK 在本地 `eidolon_dev` 提交 `596377a2eec42e5ab589e3161260234938354f02`，用 revert 撤销 `f7b4279`，保留历史。原状态保存在 `codex/sdk-permission-expansion-backup`。
- SDK revert 后的文件树与已发布 `3f98d8deed47d7e9b8ea9cb65b13d87c61174033` 完全相同。固件直接 pin 这个已发布提交；不依赖本地未发布的 revert 或扩展提交。
- 保留 `bd56a60` 的禁用 TCP 候选过滤和 `3f98d8d` 的 receive-only codec 配置。前者仍按已有证据支持的兼容性规避处理，不宣称 esp_peer 内部根因已定位；后者修复无发布时错误传入 NONE 接收 codec 的配置转换。
- 本轮没有推送任一仓库。四台设备安装的是上述生产改动提交，后续报告和测试提交不改变固件源码。

迁移使用官方已有 `on_participant_info` 的 identity/state 识别本机 ACTIVE 更新，把更新作为认证 Owner 配置可能失效的提示。它不使用 LiveKit 权限直接授权麦克风。

读取新配置后比较状态、Owner 策略、媒体参数、会话及连接材料；仅时钟观测变化不会关音频、写 NVS 或重建房间。发生实际变化才关闭旧媒体并重建。generation 防止旧房间提示污染新房间，独立通知位合并更新、避开控制命令 FIFO。已有显式 `config.refresh` 共用同一处理路径，并在拆旧连接前送出回执。

认证拉取失败保持关闭媒体，沿用已有退避；认证拒绝或不可用配置保留相应恢复/等待状态。普通确认会话保留 id 和待处理请求，按传输中断恢复；真实配置变化仍结束临时共享访问，无变化提示不结束共享访问。

## 官方当前最新

通过 SSH fetch 官方 main，并用 `ls-remote --symref` 核对远端 HEAD、main 与版本标签。官方当前 main/最新版本仍为 [v0.3.11，fbf09edf](https://github.com/livekit/client-sdk-esp32/commit/fbf09edf27a504d82119f730a35bf00fa287f7c1)。该提交已经是 fork 的祖先，官方新增提交数为 0。

因此此次不需要 merge 或 rebase，也没有改变既有线性基线。没有发现新的官方实现可以替代本轮保留的两项修改。同步走 SSH，没有依赖 GitHub HTTP/2。实际 managed header 与已发布 pin 的公共 header SHA256 相同，见 `evidence.json`。

## 回归验证

| 验证 | 结果与覆盖边界 |
| --- | --- |
| 官方公共回调布局 + 实际应用回调 | PASS，ASan/UBSan；本机/远端/状态过滤、释放 peers 锁后通知 |
| 实际配置失效处理函数 | PASS，ASan/UBSan；重复提示、旧 generation、失败关闭、认证拒绝、等待状态、共享访问、普通会话保留及回执顺序；插入终止会话的变异会被拒绝 |
| 实际 RefreshHubConfig + 实际音频 Gate | PASS，ASan/UBSan；无变化保留已打开 Gate，只更新时钟；策略变化关闭 Gate；过期版本拒绝；普通调用旧行为及恢复状态保留 |
| 连接恢复、输出策略 | PASS；时钟测试修正为有效观测，并确认两份时钟确实不同 |
| 真实 FreeRTOS/QEMU 通知队列压力 | ALL PASS；旧队列饱和可复现，新实现 24/24 控制、222/222 维护通知及 1000 次等待唤醒通过，见 `inbox.log` |
| 四板正式构建、app-only 烧录、读回校验 | 四板均完成；三块 S3 使用 IDF 5.5.4，Korvo 使用 IDF 6.1；见 `flash/results.json` |

测试入口为 `tests/run_local_configuration_callback_tests.py`、`tests/run_controller_permission_refresh_tests.py`、`tests/run_configuration_snapshot_tests.py`、`tests/run_channel_recovery_tests.sh`、`tests/run_output_policy_tests.sh`、`tests/run_controller_inbox_tests.sh`。HTTP/NVS 桩不冒充真实 Hub 或故障注入。

## BOX-3 运行中权限更新

正式固件 `3835ca3c5` + 已发布 SDK `3f98d8d`，通过现有手机管理界面完成三次禁麦/恢复循环，共六次更新，全部恢复 READY；服务端确认禁麦后无发布权限/无音轨，恢复后有发布权限/麦克风音轨。原来开启的麦克风设置已恢复，其他策略未更改。

串口抓取开始时，打开原生 USB 造成一次 `USB_UART_CHIP_RESET`，不能宣称本次整个抓取从未复位。之后六次更新期间没有额外复位：全日志仅一个 buildstamp、七次连接（首次连接加六次变更）、六次配置变化、十一次配置不变检查，RTC failure 为 0。没有出现无变化提示引起的额外房间重建。

三次禁麦的 invalidated 日志到 MicCap 停止分别约 440/400/410 ms。这是认证配置拉取完成后的时间，不是从 Owner 保存开始的整体延迟；界面操作到验收完成约 9–16 秒。配置读取仍同步占控制器约 3–4 秒，本轮没有改写为异步架构。

首次禁麦期间，两条静音 Opus 轨道分别成功订阅并释放：`TR_AypNzpbDHQ62q`、`TR_AEGiwiLd7qsqP`，期间未重新启动采集。此项验证真实接收/解码资源路径，不能代替听感、AEC 或正在交谈时切换权限的声学验收。

证据见 `permission-results.json`、`box-permissions.log`、`receive-only-probe.jsonl`、`evidence.json`。

## 启动复测：保留失败，不扩展本轮修改范围

最初四板各两轮（60 秒，末轮追加 120 秒观察），实际 **5 PASS / 3 FAIL**：

| 设备 | 第一轮 | 第二轮 |
| --- | --- | --- |
| BOX-3 | FAIL，正确解析到 .230 后 descriptor 连接超时 | PASS，READY 15.989 秒 |
| StackChan | PASS，READY 16.561 秒 | PASS，READY 15.681 秒 |
| Waveshare | PASS，READY 17.529 秒 | PASS，READY 15.409 秒 |
| Korvo-1 | FAIL，认领的 Mac Hub 主机名解析失败 | 同类 FAIL |

两台失败设备分别追加一次 150 秒启动观察：BOX-3 PASS（READY 15.799 秒），Korvo FAIL。合计十次观察为 **6 PASS / 4 FAIL**。各轮有计划复位；后续启动成功不算前一轮自然恢复。通过轮次的稳定阶段 SRAM 没有本轮持续下降证据，不据此排除长期泄漏。

BOX 失败发生在创建 SDK 房间之前；这次没有对应包级证据，不能将其直接归为之前的 SYN/SYN-ACK 接收故障。Korvo 也尚未进入 SDK 房间：Mac `app-ready` 的 `lan_name_resolves` 失败；直接 HTTPS 与正确主机名的直接 UDP 查询正常，multicast 查询失败。一次既有 Hub 单服务重启未恢复。独立接收探针能收到 RK multicast 查询，独立 zeroconf 注册探针在 asyncio/uvloop 下均正常，故没有据此修改 event loop、SDK、Host 配置或设备解析策略，也不宣称其根因已闭环。较早本机 HTTPS 超时探测受系统代理影响，已排除为 Hub 故障证据。

本轮 SDK 迁移及官方同步已完成；四板启动稳定性不能写成全部通过。此前未闭环的无线接收问题、此次 Mac Hub 运行状态问题、真实声学对话及 OTA/NVS 故障验收保持独立边界，不为它们继续增加猜测性 SDK 修改。

## 证据保存

`boot/checkpoints.json` 与 `additional-boot/checkpoints.json` 保留原始 PASS/FAIL。串口及烧录日志为选定元数据摘录；`provenance.json` 保存完整原始文件 SHA256，摘录不能重构原始全文。未提交 JWT、API secret、Wi-Fi 密码或会话音频。
