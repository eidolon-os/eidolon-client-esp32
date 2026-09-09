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

## 身份准备与手机端配合

继续使用现有 `eidolon-trust` 自定义配置 endpoint、已有物理开窗和认证会话。
不增加一个独立配置通道。

1. 手机读取目标 Owner 的签名目录和证书。
2. 第一次连接设备时，在现有 trust handover 中附带 `prepare_only: true`。
   设备验证目标、比较当前身份归属，将候选 key 写入当前事务暂存区。
3. 返回 `prepared: true`、目标 `owner_domain_id`、候选 `device_id` 和
   `identity_fingerprint`（`sha256:`）。生效 trust 与身份此时不变。
4. 手机离开设备 SoftAP 后，向目标 Host 为**候选 key**申请 voucher；
   再次连接设备，沿用 trust handover 提交 voucher 与网络配置。
5. 设备核对 voucher 的 Owner、key 与候选身份一致，然后暂存凭据和 trust。

配套实现位于 sibling `eidolon_client_mobile` 的 `device_setup_page.dart` 与
`platform_device_provisioning.dart`，保持原有“两次访问设备、中间访问 Host”的流程。
跨 Owner 或代际重置必须先准备候选身份，旧手机直接携带旧 key 的 voucher 会被拒绝；
该能力需要 ESP32 与手机端配套发布。

voucher 的 HS256 签名仍由 Hub 验证；设备只解析绑定字段并核对它们。
本地提交不意味着获得准入或 Approval。跨 Owner 后仍需目标 Owner 的标准批准。

## 一个事务、一个提交决定

| 持久化项 | 用途 |
| --- | --- |
| `eidolon_id/id_active` | 一个原子 JSON 项，包含当前 PEM、Owner/generation、key id、base id 与 voucher |
| `eidolon_id/id_pending` | 当前配置的一个候选身份；成功或取消后删除 |
| `owner_trust` 现有双槽 | 暂存和切换信任材料；完成后清理非生效槽 |
| `eidolon/ctx_record` | 当前事务的阶段、目标 Owner、SSID、快照摘要及清理进度 |
| `wifi/ctx_candidate` | 仅用于提交恢复的网络候选，含密码；成功或安全取消后删除 |

事务顺序：

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

## 历史存储与故障边界

- 旧版正常存储可读取；下一次成功配置迁移成原子身份项。
- 历史凭据、Claim 或 Enrollment 的 Owner/key/generation 与已验证目标不一致时，
  在授权配置中准备新身份；不能把这些材料继续交给新 Owner。
- 有效 `ctx_record` 在 Station 启动前自动续做；未完成时拒绝另一配置覆盖它。
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
