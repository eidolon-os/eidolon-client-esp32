# ESP32 单 Owner commissioning 与恢复

设备只保留一个生效的 Owner 上下文。Host 是逻辑 Authority 的连接地址，
不作为设备归属标识。协议事实以共享文档
`../docs/设备与Body/设备生命周期状态机与恢复边.md`、生成的 Device Foundation V1
以及 Hub Admission 的 Proposal / Decision / Grant / ACK 为准。
本文描述 ESP32 的落地方式和当前验收边界。

## 职责与变化分类

| 情况 | 设备行为 |
| --- | --- |
| 同 Owner 换 Host、IP、网络、普通固件升级 | 保留 operational key、Claim、Enrollment 和伙伴状态；AuthorityLocator 校验并更新路由 |
| 同 Owner 目录更新 | 校验签名、Owner generation 和 directory revision；拒绝回退及同版本不同内容 |
| 已验证目录推进 Owner generation | 与普通部署分开识别；旧 Claim 不再用于新代际。没有恢复授权时进入现有配置入口；已授权配置会准备新身份 |
| 已认证配置选择另一 Owner | 暂存新身份，获得目标 Owner 的 voucher，提交后执行旧归属范围清理，继续标准 Admission |
| 401、404、超时、发现其他 Owner 的 Host | 不据此清空 Claim、生成身份或转移归属 |

`OwnerTrustCommissioner` 校验配置输入；`CommissioningRuntime` 是配置动作的唯一
执行者，先等待已有 activation 写入结束并静止语音 actor，再取得无线网络控制权；
这样 generation 检查与写入之间也不会和归属提交并发。
`CommissioningTransaction` 协调已有存储，不另建生命周期服务。
`DeviceClaimConsumerCore` 负责 Enrollment / Grant / ACK。
`DeviceAuthorityLocator` 继续负责已验证目录和逻辑 Authority 的解析。

## 配网界面与准入状态

Admission 的 `PROPOSAL_EXPIRED` / `GRANT_EXPIRED`（410）及明确的 `NOT_FOUND`（404）
走现有 `RecoveryRequired`，保留恢复材料；HTTP 状态本身不代表 Claim 被撤销。
实际撤销仍走 `Revoked`，不因进入配网而恢复授权。

配置 actor 已确认 Advertising / SessionActive 并投影 `RuntimePhase::Commissioning` 时，
界面优先显示“设备配网”和现有手机添加指引。旧的撤销状态不能覆盖已经开放的配网入口；
这里只改变显示，不清除 Claim 状态、不放开会话权限。退出配置回到恢复状态后，真实撤销
仍显示“设备已移除”，普通恢复显示“需要恢复”。

## 配置退出与网络恢复

配置 actor 负责事务收敛、关闭配置 transport 和交回无线电控制权。成功提交与安全取消
都在这些步骤完成后进入 Idle；`StartStation()` 不代表连接成功，旧 AP 不可达也不能
无限占用配置租约。不可逆事务仍未完成时继续留在 RecoveringConfiguration。

连接状态由原有 WifiManager 提供，扫描与退避重试继续使用原实现。Application 在配置
退出后读取当前 Station 连接事实，补回交接期间被屏蔽的连接通知；常规网络回调进入同一
Application 队列，处理时核对配置 generation 与当前连接事实。配置 UI 快照使用既有
state_revision 拒绝迟到投影，连接恢复按 generation 与连接边沿去重。旧 activation
完成事件不能覆盖新配置，旧 worker 退出后按当前网络事实继续已有激活入口。

因此，设备从旧网络断电带到新网络时，保留 Owner、身份、Claim 与伙伴状态，显示
“寻找已保存 Wi-Fi／长按 BOOT 更换网络”，继续重试。物理在场配入新网络后继续原
AuthorityLocator 与通道恢复流程。Idle 只说明配置资源已释放；已就绪仍要求网络、
准入与业务 transport 的真实就绪证据。

## 身份准备与手机端配合

继续使用现有 `eidolon-trust` 自定义配置 endpoint、已有物理开窗和认证会话。
不增加一个独立配置通道。

1. 手机读取目标 Owner 的签名目录、证书和分页 Claim 状态；查询失败不进入设备网络。
2. 第一次连接设备时，在现有 trust handover 中附带 `prepare_only: true`。
   设备验证目标、比较当前身份归属；可续用时引用生效身份，否则准备候选 key。
   若 Host 已将该 device instance 撤销，手机在两次 handover 中均发送
   `replace_revoked_identity: true`，要求同 Owner 下也准备新身份及新 voucher。
   这是物理配置窗口内的新生命周期请求，不是恢复旧 Claim 的权限；固件仍验证
   Owner 目录、防回滚边界与 voucher 绑定，最终批准仍由 Host 决定。
   普通网络恢复入口遇到撤销记录须返回重新添加，不能静默更换身份。
   手机要求新 device id 和 `requires_voucher: true`；设备未满足则在写网络前停止。
   提交复用现有 commissioning replacement 事务：清理旧 Owner 数据/Claim、保留
   本次指定网络、切换候选身份。旧 erase 仍绑定旧 instance，不能作用到新生命周期。
   准备阶段不清除生效状态，取消可回滚；不修改或豁免 Host 的旧删除指令。
3. 返回 `prepared: true`、目标 `owner_domain_id`、候选 `device_id` 和
   `identity_fingerprint`（`sha256:`）及 `requires_voucher`。生效 trust 与身份此时不变。
   只有同 Owner、同 Owner generation、同 key 且有可续用 Claim/Enrollment 才返回 false。
4. 手机离开设备 SoftAP 后，仅在需要时向目标 Host 为**候选 key**申请 voucher；
   再次连接设备，沿用 trust handover 提交 voucher 与网络配置。
5. 设备核对 voucher 的 Owner、key 与候选身份一致，然后暂存凭据和 trust。
   续用身份接受无 voucher 的既有 Stage 路径；旧手机仍发送 voucher 时核对绑定，
   不为新的 jti/exp 改写现有身份。相同 trust bundle 也只引用已有槽。

配套实现位于 sibling `eidolon_client_mobile` 的 `device_setup_page.dart` 与
`platform_device_provisioning.dart`，保持原有“两次访问设备、中间访问 Host”的流程。
跨 Owner 或代际重置必须先准备候选身份，旧手机直接携带旧 key 的 voucher 会被拒绝；
该能力需要 ESP32 与手机端配套发布。
新手机对没有 `requires_voucher` 字段的旧固件仍默认申请 voucher。
本地 Claim 的续用不等于服务端仍批准：联网后继续既有撤销检查和 erase 接收流程。

voucher 的 HS256 签名仍由 Hub 验证；设备只解析绑定字段并核对它们。
本地提交不意味着获得准入或 Approval。跨 Owner 后仍需目标 Owner 的标准批准。

## 一个事务、一个提交决定

| 持久化项 | 用途 |
| --- | --- |
| `eidolon_id/id_active`、`id_pending` | 两个物理身份槽；每槽一个原子 JSON，保持 key/base 一体 |
| `eidolon_id/id_slot` | 一个 u8 选择当前槽；旧存储无此键时读取槽 0（id_active） |
| `eidolon_id/id_stage` | 当前会话 generation、候选槽及是否引用活动身份；不复制 key/base |
| `owner_trust` 现有双槽 | 暂存和切换信任材料；完成后清理非生效槽 |
| `eidolon/ctx_record` | 当前事务的阶段、目标 Owner、SSID、快照摘要及清理进度 |
| `wifi/ctx_candidate` | 仅用于提交恢复的网络候选，含密码；成功或安全取消后删除 |

需要更换身份的事务顺序：

```text
准备身份和 voucher → 暂存 trust → 验证 Wi-Fi 与 Owner 路由
  → CommitDecided 持久化
  → NetworkCommitted
  → OwnerCleaned（同 Owner 普通更新跳过清理）
  → IdentityCommitted
  → TrustCommitted
  → 清理暂存/旧槽/旧版键 → 关闭配置连接 → 恢复运行
```

`CommitDecided` 是不可回滚边界，早于任何旧归属清理。此前取消只丢弃暂存；
此后取消、断网和重启都继续同一事务。密码留在 Wi-Fi 存储中，事务记录只存摘要。
网络写入需要读取 NVS 验证，不能以 SsidManager 内存列表作为落盘证据。
身份提交只验证已准备的记录并切换 `id_slot`，不在决定之后再次分配整份身份。
清理必须按当前槽删除另一槽，不能再假设 `id_pending` 永远是废弃候选。

不换身份的事务不做 Owner 清理，也不逐阶段重写 journal；保持一份不可变快照，
以持久网络、身份和 trust 的实际摘要判断哪些动作已完成。`setup_generation` 仅用于
会话关联，可以跨重启重复；它不是 Owner generation，不能据历史 generation/replacement
判断本次是否换身份或需要重启。本次结果明确返回 `identity_replaced`。

跨 Owner 与授权代际恢复复用 `EspIdfDeviceLocalEraseAdapter` 的范围清理计划：
清理旧 Claim、Enrollment、handoff key、配置、伙伴偏好、Owner 人脸数据和旧网络，
保留事务及待提交身份/trust，逐目标记录进度。全部提交并关闭配置传输后自动重启，
让旧会话和内存缓存退出。同 Owner 普通换网无需身份轮换或此重启。
没有整片 NVS 擦除，没有长期 A/B Owner 档案。A→B→A 的最后一个 A 使用新生命周期。

本地切换不依赖旧 Host 在线，也不代表远端旧 Owner 的记录已被删除。
这不是共享合同中尚未完整落地的正式 Transfer 协议。

## Enrollment 与迟到响应

继续领取/ACK 前核对 Owner id、Owner generation、DeviceInstance 和 operational key。
网络请求前后检查当前配置 generation；解密、签名、落盘和销毁 handoff 材料时
再次检查上下文。新配置开始后，旧响应不能写入新状态。

Grant 先持久化，再 ACK。ACK 后先持久化 ActiveClaim，再销毁 Enrollment 材料；
任何中断都通过原有 `ResumePending` 继续。恢复已有 Enrollment 不生成替代 handoff key；
终态清理先区分密钥缺失与存储读取失败，缺失按幂等成功处理。已有 ActiveClaim 也要完成遗留的
Enrollment 清理，不能直接跳过。

OwnerChanged、AuthorityReset、AuthorityRollback、ForeignPrincipal 和 OwnerRevoked
分别分类。Owner/generation 不匹配不再删除 Claim 并沿用旧身份重新申请。
配置恢复状态也不再显示成“已被 Owner 移除”。

**Proposal 在 Authority 侧结束（`PROPOSAL_EXPIRED` / `GRANT_EXPIRED` / `NOT_FOUND`）不是 Owner 说不。**
2026-10-10 之前，collect/ACK 收到这些答复会保留 checkpoint、进入 "Waiting for Owner to restore
device access" 终态——而 Hub 并没有这条恢复路径，设备从此既不重申请也批不了（korvo-1 真机复现：
15 分钟内 Owner 的手机没能到达 Host）。现在 `AbandonFinishedProposal` 调 `AbandonPendingProposal()`
丢弃 checkpoint（销毁本次 handoff 材料），并在同一次 attempt 内用 `enrolled-base-key-v1` 重新 propose；
Hub 按 Owner 在配网时记录的 standing 自动裁决（见 `eidolon_hub/docs/adr/20261010-admission-standing-at-issuance.md`）。
只有放弃不成立时——Claim 已激活（对 Proposal 的答复不是对 Claim 的授权）或存储失败——才仍进入
等待 Owner 的恢复态。Owner 真正的拒绝/移除仍由 Hub 的 `requires_fresh_presence` 在 propose 时以 403 答复，
不会进入这条路径。

## 历史存储与故障边界

- 旧版正常存储可读取；下一次成功配置迁移成原子身份项。
- 历史凭据、Claim 或 Enrollment 的 Owner/key/generation 与已验证目标不一致时，
  在授权配置中准备新身份；不能把这些材料继续交给新 Owner。
- 有效 `ctx_record` 在 Station 启动前自动续做；未完成时拒绝另一配置覆盖它。
  失败后明确显示恢复受阻，停止无状态变化的定时重放；长按重试同一决定。
  `RequestOpen()` 入队不等于成功取得无线电，不能据此停止其他生命周期的定时器。
  UI 只有在实际请求恢复 Station 后才显示返回网络，收到扫描事件才显示寻找 Wi-Fi。
- 旧 `id_pending`/`ctx_record` 可由新槽位机制直接完成，不清缓存、不擦身份。
  槽 1 激活后，旧固件不知道选择器，不能直接降级至不支持槽位的固件。
- 只有旧版 `ctx_gen` 的中断没有可信身份快照，不能自动回滚到猜测的旧归属。
  物理打开的认证配置可以用新 voucher、新身份完成目标生命周期，提交后清除旧记录。
- 无法读取 NVS、损坏的新事务快照不能被当作“首次配置”或“事务不存在”。
  保留现场并阻断运行。没有足够证据时不能承诺自动重建丢失的身份。

## 验证与尚未通过的验收项

主机测试：`run_commissioning_transaction_tests.sh` 使用真实事务、凭据适配器、
voucher parser 和清理计划，替换 NVS/硬件端口，逐个持久化写入点注入断电。
覆盖提交前取消、提交后重启、同 Owner 更新、代际更新、A→B→A、旧事务和存储损坏。
Claim 测试覆盖外 Owner、外 key、外 DeviceInstance、旧代际与迟到 Grant/ACK。
commissioner 测试覆盖 prepare 不提交 trust、目录反回退；actor 测试覆盖提交后取消
不能触发回滚。另运行 AuthorityLocator、范围清理、物理恢复、通道恢复等回归。
手机测试核对准备后的 key 被用于 voucher，且请求 Host 时已离开设备连接。

`run_commissioning_nvs_replay_tests.sh` 使用 ESP-IDF 5.5.4 的原版 NVS 存储实现，
在 16 KiB 合成满分区上先证明旧身份复制失败，再运行生产事务/凭据代码恢复，
逐个 Flash 写/擦点断电重启，以及跨重启 100 次完整网络维护事务。
需要 `IDF_PATH`、C++ 编译器、pkg-config、cJSON、OpenSSL、zlib；无参数不需要现场数据。
可选参数为从 Flash 0x8000 开始的私有镜像；镜像含密钥和密码，不能提交仓库。
Trust/Claim/SSID 硬件端口仍为测试替身，不将此测试称为全部驱动或证书校验的实机验收。
上述回归已纳入 CI 的 commissioning-recovery job。

这些是主机故障注入和构建验证，不等同真机电源中断、Wi-Fi 驱动和手机 SoftAP 验收。
尚需按共享真机计划完成各阶段断电/迟到响应，以及最终伙伴分配、对话链路联调。

**服务端缺口：过期 Grant 的原批准重签发。** 当前 Hub Admission 的 collect/ACK
对过期 Grant 返回 `GRANT_EXPIRED`，尚未提供共享合同要求的基于同一 Decision
重签发流程；Proposal 超时还可能先返回 `PROPOSAL_EXPIRED`。
设备现在保留 Enrollment 和 handoff 材料，显示等待 Owner 恢复准入，
不会清空材料并制造第二次批准。要通过“批准后任意长时间断电，恢复后自动完成”
验收，需要 Hub 补齐合同定义的同 hardware/manifest/Owner 重签发，并联调设备续领。
本次没有通过修改 Decision 或重建 Claim 来绕过这一缺口。

## Channel Assignment

Claim 生效后从逻辑 DeviceControl 拉取配置。Hub 的 `opaque_binding` 继续由 Provider
定义，设备只消费 `application/vnd.eidolon.livekit-device+json;v=1`。
PendingApproval、WaitingBinding、Active、Revoked 保持既有语义；本地 RecoveryRequired
不授权使用缓存会话。Enrollment、伙伴分配与可对话分别需要各自的权威事实，
commissioning 成功不能代替任一项。

本次验证记录（2026-09-10）：21 组相关主机测试通过；配套手机配置测试
34 项通过、1 项既有跳过，修改涉及的 6 个 Dart 文件静态分析通过。
M5Stack StackChan 与 ESP-BOX-3 / ESP32-S3 配置均生成完整固件并通过分区大小检查。
随后使用 `scripts/eidolon/eidolon-esp-box-3.sh` 的构建、烧录和 `run_idf erase-flash`
流程完成 BOX-3 整片擦除与烧录，写入校验、启动指纹和 ELF 摘要核对通过。
串口确认设备以无 Owner、无已存 Wi-Fi 状态启动，并在收到配置请求后完成事务提交、
连接 Wi-Fi。尚未验证这次运行的最终 Claim、伙伴分配和对话链路，也未执行
真机电源/无线故障注入。LiveKit 固定提交未变。

本轮网络交接回归覆盖：已提交但 AP 消失后可重新开窗、清理前的提前模式回调、
新 generation 拒绝旧候选回调，以及连接先于/晚于租约释放、同代次重复通知和跨代次
重新唤起运行时。对应 `run_commissioning_orchestrator_core_tests.sh` 与
`run_operational_readiness_tests.sh`。这些为主机测试；本轮改动仍需 BOX-3 真机验证。


2026-09-19 BOX-3 回归：保留原故障存储，仅更新应用，启动自动完成旧事务；
用户换网后取得 IP，pi5 的既有 erase 收到 ERASED ACK。用户再次长按重新认领，
新 Claim 激活，分配伙伴后 LiveKit Connected 并出现 speaking/listening 事件。
此次身份更换由服务端此前已撤销并排队的 erase 引起，事务恢复本身没有强制轮换。
完整调查、原版 NVS 回放与实机证据边界见
[BOX-3 recovery 报告](../reports/box3-recovery-20260918/analysis.md)。

## 普通升级、NVS 故障与本地恢复

普通 app OTA 不更新分区表，不清空配置。Hub 固件启动时 NVS 返回“无空闲页”或“新版本格式”
不再擦除默认 NVS：设备身份私钥（`eidolon_id`）、Wi-Fi 与 Claim 都在其中，擦除等于销毁设备归属。
初始化失败时记录 `boot_storage_error`，不启动 Wi-Fi，显示恢复界面，长按 BOOT 重启重试；
持续的存储损坏或较新格式需要安装兼容固件维护，不能靠重新配网重建已丢失的私钥。
owner_trust 分区小于 64 KiB 不再被拒绝初始化：64 KiB 是新布局预算，不是旧分区的运行时门槛。

Settings 不再先删除旧值为新值腾空间；字符串、整数和布尔写入失败均由同一对象的 Commit 返回错误，
删除失败同样返回错误，不再 `ESP_ERROR_CHECK` 重启。证书与目录字符串最多 3999 bytes（另加 NUL），
在协议入口检查，避免写到一半才发现超出 `nvs_set_str` 的上限。
Commit 不提供多键事务回滚；NVS 已完成的写入可能已经持久化，不能用失败后的 Commit 撤销。

OTA 确认不再等 Wi-Fi：板级/UI 初始化完成后确认本地可执行程序已启动
（`main/eidolon/firmware_boot.h`）。持久数据不决定镜像是否有效：事务恢复仍由
`WifiBoard::StartNetwork` 执行，身份、签名和目录有效时间仍由原有配网与准入流程验证。
确认固件不会使故障事务、无效身份或过期目录取得联网授权，也不会在检查过程中迁移 Owner 数据。

NVS 初始化失败时，在板级启动前确认并保留当前恢复镜像，阻止长按重试退回会自动擦 NVS 的旧版本。
不能将 IDF 的“存在可启动回退镜像”当作“旧镜像兼容并保护当前数据”的证据。
存储故障和目录过期均不主动触发应用回滚；正常路径在完成本地启动前崩溃，仍由 IDF bootloader
按原有 pending-verification 规则回退。otadata 查询/确认写入失败会记录错误，不能宣称已成功保留镜像。
存储恢复界面的长按重试会再次确认镜像；确认仍失败时保留恢复界面，不主动重启进入旧镜像。

旧布局 `p256_priv` 与槽位身份一样，在返回 Loaded 前验证私钥可解析；损坏返回 Unavailable，
保留原值且不自动生成替代身份。新设备确实没有身份时仍返回 NotFound，沿用原有首次配网流程。

Owner trust 的存储选择与迁移保持原样（见上文）；trust key、selector、digest 与身份/事务的
version 1 编码均未改变。格式兼容不代表任意旧固件的故障处理策略安全，不能据此自动降级。
