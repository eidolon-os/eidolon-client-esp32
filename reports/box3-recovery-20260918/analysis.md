# BOX-3 recovery 根因调查（2026-09-18）

## 结论与证据等级

已定位当前设备 commissioning 无法前进的确定性阻塞：默认 16 KiB NVS 中，`id_pending` 已持久化，但 `CommitStaged()` 再复制成 `id_active` 时，字符串写入需要连续 37 entries，GC 后最多仅能提供 33。使用设备 Flash 镜像和 ESP-IDF 5.5.4 原版 NVS 存储代码，在主机内存重放该写入，稳定返回 `ESP_ERR_NVS_NOT_ENOUGH_SPACE (0x1105)`。

这不是私钥丢失、Owner generation 不匹配或候选材料丢失。更深层的问题是：普通同 Owner 重配也重新签发 voucher、复制整份身份，然后在提交决定之后还需要一次大记录分配；任何失败又被全局解释为禁止联网、禁止重新设置。容量/分配失败因此升级成设备生命周期死锁。

初次调查阶段未刷修复固件，未在设备上执行写入、擦除或身份重置。后续实施与验收见本文末尾。串口打开时发生 USB_UART_CHIP_RESET，随后 esptool 读取 Flash 时也复位了设备。重启前状态没有完整串口记录；重启后的恢复循环有实测日志。长按后错误文案的链条由用户观察和当前实际版本对应的代码共同支持，未在本调查中再次触发按键。

## 实机状态

固件 buildstamp：`673c91a80`，BOX-3，ESP-IDF 5.5.4。当前仓库后续的 identity 错误日志改动尚未刷入；涉及本次提交、恢复及 UI 的关键逻辑相同。

启动日志每约 3 秒出现：

```
WifiBoard: Commissioning recovery pending; retrying before Station start
```

Flash 分区表/NVS 区间只读导出：从 0x8000 开始 0x18000 bytes，包含默认 NVS 和 owner_trust。原始镜像含私钥与 Wi-Fi 密码，仅留本机 `/private/tmp/eidolon-network-investigation-flash.bin`，权限 0600，未纳入仓库或报告。串口日志位于 `/private/tmp/eidolon-network-investigation-serial.log`。

| 项目 | 现场结果 |
|---|---|
| ctx_record | 有效记录，phase=3 (OwnerCleaned)，generation=1，replacement=false，cleanup_index=0 |
| id_active | setup_generation=2，replacement=true，ready=true，1147 bytes 含 NUL |
| id_pending | setup_generation=1，replacement=false，ready=true，1148 bytes 含 NUL；原始摘要符合 journal |
| 新旧 identity | 私钥、key_id、base、Owner、owner_generation 全部相同 |
| identity 不同字段 | setup_generation、replacement、voucher |
| voucher 不同 claims | 仅 jti、exp |
| trust_active / trust_stage | 槽 1 / 槽 0；trust_sgen=1 |
| 两个 trust 槽 | Owner/root/auth/descriptor 均相同；两槽 bundle hash 均符合 journal |
| Owner generation | active、pending、两个 descriptor、本地 Claim 均为 8 |
| directory_revision | 两个 descriptor 均为 1 |
| 本地 Claim | active，claim_generation=1，trust_epoch=1；Owner/instance 与身份一致 |
| Wi-Fi | ctx_candidate hash 符合 journal；保存的 SSID/password 与 candidate 一致 |
| 相关 NVS 条目 CRC | 检查通过 |

这里的 Claim active 是设备本地证据，不代表已经向在线 Hub 再次确认未被撤销。恢复网络后仍应走已有 Hub 校验与远程指令收取流程。

## 原版 NVS 重放

`replay/run.sh` 编译安装在本机的 ESP-IDF 原版 `nvs_storage.cpp`、`nvs_page.cpp`、`nvs_pagemanager.cpp`、`nvs_types.cpp`、`nvs_item_hash_list.cpp`。仅替换底层 flash 为内存模拟、关闭日志并用 zlib 提供 CRC；没有重写 NVS 分配算法。每个实验从同一原始镜像重新开始，检查 flash 的 1→0 写入规则。

| 实验（只修改内存副本） | 结果 |
|---|---|
| 将真实 id_pending 写为 id_active，连续重试 4 次 | 每次 0x1105；每次 GC 擦除 1 个扇区 |
| 先删除 eidolon/config 缓存，再执行同一写入 | ESP_OK |
| 为模拟分区增加 1 个空白页，再执行同一写入 | ESP_OK |
| 不复制身份，只试写一个 uint8 slot selector | ESP_OK |

后三项是定位与方案可行性实验，不是完整迁移/整笔 Finish/真机端到端恢复验证。没有把这些改动写回设备。

默认 NVS 四页状态：三页有效条目数分别 95、117、93，另一页为空白 GC 保留页。每页 126 entries。GC 最佳选择仅能产生 `126-93=33` 连续空位；1148-byte 字符串需要 `1+ceil(1148/32)=37`。NVS 先写新值，再删旧值；不会先删除旧 id_active 腾空间。多次重试不能改变这个条件，却会重复回收。

现有 `tests/run_commissioning_transaction_tests.sh` 通过。但其 NVS 是无限容量 std::map，nvs_set_str 总是成功（除模拟掉电）；不覆盖真实页面容量和字符串连续空间。这解释了事务测试通过而真机卡死。

## 两种屏幕是同一个故障

1. 重启：`WifiBoard::StartNetwork()` 先恢复事务。phase=3 调 `CommitStaged()`，大记录写入失败，返回 false；在 `TryWifiConnect()` 前 return，显示 Restoring connection，3 秒再来。
2. 长按：`RequestOpen()` 仅代表请求入队；WifiBoard 立刻停止上述 timer。
3. actor 在 EnsureIdentity 阶段读到未完成 ctx_record，拒绝开窗，经 IdentityFailed 回 Idle。此时还没有获取 radio，也没有启动 Station。
4. `Apply(OpenRequested)` 曾根据“有保存的 SSID”设置 previous_station_mode。Idle UI 把这一意图当作已恢复 Station，显示 Looking for saved Wi-Fi。
5. 因恢复 timer 被停掉，既不恢复、也不扫描；重启重新从第 1 步开始。

因此该屏幕不能证明已开始扫描，也不能证明最近一次重新配置成功。历史上生成当前 journal 的那次事务至少已走到网络验证/提交，但其后的开窗操作可能在准备阶段就失败。

## generation 与跨项目边界

- `setup_generation`：CommissioningOrchestratorCore 的本地会话计数，进程重启从 0 开始，再 ++。active=2、pending=1 合法，恢复比较 pending 与 journal 的 1，不要求大于旧 active。没有证据表明版本倒退拒绝导致本次故障。
- `owner_domain_generation`：Owner 生命周期边界。现场全部为 8，不是 Authority reset。
- `directory_revision`：信任目录修订。现场新旧都为 1，正文也相同。
- `claim_generation/trust_epoch`：准入与权限围栏。不能用本地 setup_generation 替代，也不应为换网递增或删除。

当前 identity 编码却同时保存上述稳定身份、易变 voucher、setup_generation、replacement。即便身份不变，重配也改变整个记录的摘要并强制重写。这不是 generation 比较错，而是会话元数据和稳定身份生命周期被绑进了同一个提交对象。

查阅项目：

- ESP32 `owner_trust_commissioner.cc` / credential store：同 Owner 同代际不会要求换 key；已有 voucher-free 的 Stage(nullptr) 与有效 Claim/Enrollment 续用判断，但仍会构造完整 pending 身份。
- mobile `device_setup_page.dart`：prepareOwner 后无条件向 Host 申请 voucher；coordinator 的 provisionAndAdmit 强制要求 voucher，再交给设备。
- admin `commissioning_vouchers.py`：为同一个 base/key 重签时产生新 jti/exp；与现场记录吻合。
- Hub `admission/application.py::base_identity_for_key`：按已认证 key 查已签发 base，允许同一身份续用；没有证据表明本次被迫换 base。
- ESP32 `hub_onboarding_client.cc::RunAccepted`：已有本地 Claim 的续用与服务端校验路径，不必每次换网重新 Proposal。
- ESP32 `OwnerTrustStore::CommitStaged`：已经实现“双槽准备、切换小标记”，身份提交可以复用这一模式。

没有访问在线 Hub 数据库，不能据此断言服务器当前的撤销/erase operation 状态；但本次阻塞发生在联网以前，真实镜像可独立复现，不依赖这一未知量。

## 推荐的设计简化

### 1. 普通网络维护不再等于重新创建设备身份

使用现有 Owner/key/Claim 分类决定本次 setup 的意图：同 Owner 同 generation，完整匹配的身份与可续用 Claim/Enrollment，走现有网络配置与运行态恢复；保留身份和 Claim，不重写私钥/base，不因为开了一次窗口而换 voucher。相同 trust bundle 无需重复 stage。

首次接入、换 Owner、Owner reset、需重新授权的 revoked/rejected 等情况，才进入完整身份/准入路径。物理在场只授权相应操作，不自动把每次网络维护升级成换身份。

复用现有 prepareOwner、Stage(nullptr)、ClassifyStoredClaim、RunAccepted；给 prepare 结果明确是否需要新准入凭据，mobile 据此选择，去掉 coordinator 对每种配置都必须有 voucher 的要求。不能只在手机跳过 voucher，设备端也必须跳过不变身份的复制。

setup_generation 留在会话/事务关联中，不作为稳定身份的版本；持久恢复沿 journal 的确定候选及摘要进行。跨重启需要唯一关联的地方，优先复用已有 session_id，不再发明一个“全局超级 generation”。

### 2. 真正需要换身份时，提交激活已准备的数据，不再复制一遍大记录

借鉴仓库已有 OwnerTrustStore 双槽设计：候选的全部身份材料先写入非活动槽并校验；提交切换小型活动标记，而不是将 id_pending 再完整写成 id_active。key/base 仍在同一原子记录中，保持 R23。

大数据写入/验证在不可逆决定前完成，提交尾部的小标记和必要 journal 更新也必须有明确空间预算。缓存不得挤占完成事务的空间。无需增加新事务 actor、重试队列或通用状态机，也不以单纯扩大默认 NVS 作为修复结论。

voucher 属于准入过程数据，不应使稳定身份的每次续用都复制一份密钥。现有 ForgetSpentVoucher 未见调用；即便接上，也需受事务完整性约束，不能在未完成 journal 引用其摘要时任意改 active。

### 3. 去掉过宽的恢复限制，保留真正的信任边界

去掉“存在任意 ctx_record，就禁止任何联网/设置”的一刀切：对于经完整校验证明 Owner/key/base/trust 未变化、未执行破坏性清理的网络维护，可以恢复一致的现有运行态。不能仅凭 replacement=false 或一个 phase 放行。

完整身份替换尚未完成、撤销、半身份等仍不能复活旧 Owner。保留 D8、R23、R25b；去掉这些不会修复分配失败，还会引入接管或身份混用。

恢复的定时任务只有在 actor 实际接管对应工作后才移交；开窗失败必须保留原失败原因。Idle 只表示 actor 空闲，不能推出 Station 正在扫描。保留具体 NVS 错误，空间不足不要无条件反复 GC。

## 当前设备的恢复方案

事务恢复本身无需整体作废、换 device_base_id 或清除 config 缓存。最终实现直接激活已有候选槽，清理旧槽后完成原事务。设备真实镜像已通过生产代码重放：19 次 Flash 写/擦动作逐点中断均能恢复；完成后跨重启连续执行 100 次完整网络维护事务，身份保持不变。

2026-09-19 现场追加核对：只读查询 pi5，确认当前实例的服务端 Claim 已于 2026-09-18 09:53:27 UTC 撤销，Owner generation=8、claim_generation=1、trust_epoch=1；对应 erase 为 pending、attempt_count=0，截止时间为 9 月 25 日。此前“本地 Claim active”不代表服务端仍批准。新固件恢复连接后，仍须执行既有移除决定，不能用维修绕过撤销。用户已明确授权刷入修复、执行该移除；之后重新认领是服务端决定的后果，不是本地事务修复强行轮换身份。

## 对上一轮推断的纠正

- 这次没有候选材料蒸发；失败是仍需分配的身份复制。
- phase>=NetworkCommitted 不等于半身份。当前 phase=3、replacement=false，身份和 Owner 都仍一致；按此阈值整机作废会误伤。
- phase=CommitDecided 也不保证网络未改。AddSsid 在 Advance(NetworkCommitted) 之前，掉电/写失败时 journal 可以留在旧 phase。因此不能仅按 phase 放开所谓安全 rollback。
- claim 创建日期不能证明 journal 不是 Legacy；现场读取才是证据。本次读取已证实为有效新格式 journal。

## 验收

用真实 NVS 算法而非无限 map：同 Owner 重配、重启后 generation 重用、逼近容量、提交逐写掉电；验证身份/base/Claim 不变，失败保留可用一致配置，永久错误不会循环擦写。完整新生命周期继续验证撤销/Owner reset/半身份边界。UI 必须分别验证“开窗拒绝”“扫描开始”“候选配置已提交”，不能用同一条通用连接文案覆盖失败。

## 已实施的机制修复（2026-09-19）

- 同 Owner/epoch/key 且有匹配 Claim/Enrollment 时，以小型会话标记引用现有身份；相同 trust 复用活动槽。手机依据认证 prepare 的 requires_voucher 选择是否申请 voucher，缺失字段仍默认需要。
- 身份沿用现有 id_active/id_pending 两槽，用 id_slot 原子切换；兼容旧 journal。提交不再复制完整私钥记录；明确拒绝选择器缺失记录时回退到旧 key。
- 非替换事务保持不可变 journal，以持久数据摘要重放，不再推进五段日志。完整 Owner 替换保留既有范围清理协议和逐目标进度。
- 本次是否替换身份作为事务结果传递，不再用历史 setup_generation/replacement 推导。显式替换不能被之前的复用候选吞掉。
- 固定错误停止定时重放；长按重试同一决定。开窗请求入队不停止网络计时；恢复失败保留具体 UI，扫描来自实际网络事件。
- 原始私有镜像、旧应用备份保留本机，不提交仓库。没有扩大 NVS、增加新生命周期服务或另建配网协议。

## 自动验证结果

- 10 组相关固件主机测试通过（事务、actor、commissioner、transport、voucher、trust policy、erase adapter/core、physical recovery、claim recovery）。
- 原版 ESP-IDF NVS 合成满分区先复现旧写入失败，新恢复 14 个物理动作逐点断电通过；真实镜像对应 19 个动作。两者均通过 100 次完整换网事务跨重启运行。
- 手机端相关测试 38 项通过、1 项既有跳过；变更的 6 个 Dart 文件静态分析通过。
- BOX-3 完整固件构建和分区大小检查通过。现有依赖解析的 Kconfig 缺失由关闭“检查新依赖版本”解决：IDF_COMPONENT_CHECK_NEW_VERSION=0，仍使用原有锁定依赖；未修改用户已有 esp_video 版本约束。
- CI 增加原版 NVS 回放与事务/物理恢复回归。此处的断电为主机 Flash 端口故障注入，不等同真机反复断电或手机实操验收。

## BOX-3 实机验收进展

经用户授权，先备份旧应用，再仅写入 ota_0（0x20000）的修复应用。Flash 写入 hash 校验通过，未写分区表、NVS、owner_trust、otadata 或 assets。新固件启动 ELF SHA 前缀 `8a4cd7ba1`。

- 启动约 1.25 秒后直接进入 `Starting WiFi connection attempt` / `Starting station`，原恢复循环消失。随后一次串口重新打开触发的重启也正常开始 Station，说明恢复结果已持久化；现场尚未执行真实断电压力测试。
- 用户长按 BOOT 后进入 advertising，手机建立 session-active；完整重新配网在约 58.7 秒进入 committed=1，然后关闭配置 actor、归还 16 KiB 内存。
- 约 63.5 秒取得 IP 并开始 Hub activation；约 67.9 秒报告 `Device removal completed; operational runtime is fenced`，UI 为 REMOVED。
- pi5 只读复核：旧实例的 erase 从 pending / attempt_count=0 推进为 acknowledged / attempt_count=1 / result_code=ERASED。Claim 仍为 revoked，符合服务端已有移除决定。
- 用户再次长按后生成新生命周期并开启 generation=2 的配置窗口；新配网成功，EnrollmentProposal 进入待批准。约 228.7 秒记录 `Canonical ClaimGrant acknowledged and Claim activated`。
- 通过串口实际新实例 ID 查询 pi5，新 Claim 为 active、Owner generation=8、claim_generation=1、trust_epoch=1；旧 Claim 保持 revoked。
- 用户完成伙伴分配后，约 255.3 秒 LiveKit room 进入 Connected，随后观察到 speaking/listening 和 agent 音频状态事件。端到端配网、已授权移除、物理重新加入、批准、分配和语音连接已走通；音质及长时间稳定性不是此次短程日志所能证明的。
- 实机使用现场手机应用完成，因此也验证了固件对旧手机仍发送 voucher 的兼容路径。手机仓库中跳过不必要 voucher 请求的更新已通过测试与静态检查，本次未另行安装手机应用。

原始串口日志仅留本机 `/private/tmp/eidolon-box3-recovery-after-flash.log` 和 `/private/tmp/eidolon-box3-recovery-live.log`；未把原始现场网络信息和私有镜像提交仓库。
