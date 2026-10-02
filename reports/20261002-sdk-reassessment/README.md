# LiveKit ESP32 SDK 修改必要性复核（2026-10-02）

范围是本任务新增的 bd56a60、3f98d8d、f7b4279。旧分支已经存在的其他 SDK 修改不计入这三个提交。此次是源码、官方基线和既有证据复核，并增加一个官方回调边界实验；没有重新烧录或更改设备策略。

## 结论

| 提交 | 性质 | 必要性与处理结论 |
| --- | --- | --- |
| bd56a60 | TCP 禁用配置下过滤 TCP ICE 候选 | 有实际故障及修复后证据支持的兼容性规避，当前保留；不宣称已定位 esp_peer 二进制内部根因，后续官方底层修复后应复测并删除重复规避 |
| 3f98d8d | 接收模式独立设置音频接收能力 | SDK 配置转换缺陷：无发布时接收端 codec 为 NONE，不满足 esp_peer 接口；对当前无麦克风发布、仍接收 Opus 的功能必要，保留最小修复 |
| f7b4279 | 公共 ParticipantInfo 增加 locality 和权限字段 | 可替代的 API 扩展，未证明不可替代，不应作为长期必需的 fork 修改；迁移应用配置刷新机制并验证后撤销生产依赖 |

不能把三项一律叫作“找到根因后的必要 SDK 修复”。尤其此前权限部分的根因描述需要修正。

## 官方基线与职责边界

通过 SSH 只读 fetch 官方 main，核对提交 fbf09edf27a504d82119f730a35bf00fa287f7c1（Release v0.3.11）。核对的是 Git 源码，不依赖搜索引擎缓存的代码日期。

- [官方媒体配置转换](https://github.com/livekit/client-sdk-esp32/blob/fbf09edf27a504d82119f730a35bf00fa287f7c1/components/livekit/core/livekit.c)：发布音频时才填充 audio_info，订阅音频只设置 RECV_ONLY；ParticipantInfo 转换不公开权限，但仍调用 on_participant_info。
- [官方参与者更新处理](https://github.com/livekit/client-sdk-esp32/blob/fbf09edf27a504d82119f730a35bf00fa287f7c1/components/livekit/core/engine.c)：收到参与者更新时将本地和远端事件都送到回调，没有按公共字段变化过滤事件。
- [官方公开接口](https://github.com/livekit/client-sdk-esp32/blob/fbf09edf27a504d82119f730a35bf00fa287f7c1/components/livekit/include/livekit.h)：应用已有 identity、sid 和回调；没有独立的订阅音频编码配置项。
- 官方和 fork 的 esp_peer 约束都为 ~1.5.5，并非 fork 改成另一条版本线。当前主项目实际安装的是 1.5.6。不能把官方约束写法误当成精确锁定 1.5.5。

Owner 配置语义、变更通知、媒体图生命周期属于应用及共同业务层；SDK 负责正确实现收发配置和传输事件。公开更多 LiveKit 权限信息可以是合理功能提案，但不等于 Owner 配置同步只能靠它实现。

## bd56a60：保留，但降为有证据支持的兼容性规避

既有记录 reports/20260930-recovery-review/evidence.json 的 rtc_failure_order 展示：首次连接 TCP 候选先到，随后出现失败状态、又进入配对，但上层已收到失败并拆除房间；下一连接 UDP 先到则成功。此记录支持候选顺序相关的故障，单条时序日志本身不能证明 esp_peer 内部是哪一个分支出错。

该修改只在本来已禁用 TCP 的情况下，不将 TCP 候选交给底层；没有延迟失败事件、吞掉所有 RTC 错误、加入板子特例或强制重试。在 TCP 支持开启时仍保留候选。SDK 默认配置的 TCP 原先就是 false。

修复后 BOX-3 12 轮记录中 RTC failure 为 0，包含 2 轮 TCP 候选先到。此证据支持当前环境中规避有效，不证明所有 ICE 模式均已验证，也不是底层源码修复的证明。

[Espressif 官方 changelog](https://github.com/espressif/esp-webrtc-solution/blob/main/components/esp_peer/CHANGELOG.md) 的 1.3.4 已提及不接受 TCP 候选时的连接失败修复，1.5.6 又提及 TCP/UDP 同 IP/端口映射修复。这说明底层本来应处理相关情况；这些文字不足以证明本次现场与其中任一条是相同缺陷，也不能仅凭 changelog 断言本地过滤已经重复。当前安装的正是 1.5.6，仍需带确切候选、底层状态和最小复现才能向维护者归因。

移除条件：在相同依赖、关闭过滤的固件中，受控 TCP-first、UDP-first 场景均不再出现提前失败，且正常失败仍能恢复。没有该对照时直接删除会重新暴露已经观察过的问题。

## 3f98d8d：必要的接收配置修复

源码链条完整：

1. 应用用 publish.kind=NONE 禁止本地采集和发布，用 subscribe.kind=AUDIO、renderer 接收音频。
2. 官方 populate_media_options 因无发布保留 audio_info.codec=NONE；即使应用提供 publish.audio_encode，kind=NONE 时也不会采用它。
3. 官方 peer_create 将该 NONE 复制到 RECV_ONLY peer。
4. 当前 esp_peer 的 include/esp_peer.h 明确要求 audio_dir 非 NONE 时配置 audio_info，包括 recvonly。

既有 receive-only-before.log 多次记录 subscriber `Failed to open peer`。修复后 receive-only-after.log 及 evidence.json 记录无 MicCap 启动、成功订阅/释放两条静音 Opus 轨道、无本轮错误。此测试是诊断固件下的收音关闭、接收/解码通路验证，不等于真实听感、所有 codec 或最终正式版本的完整声学验收。

修改只补 peer 局部接收配置，保持 engine 的发布 codec=NONE。把发布标记设为 AUDIO 或创建假 capturer 来绕开问题会违背禁麦要求，并可能启动采集/发布，不能作为正确替代。现有公开接口没有传入独立接收 codec 的路径。

OPUS/48000/mono 是当前项目接收场景的最小配置，不把它称为完整通用音视频能力协商。官方增加正确的接收能力配置后应改用官方实现。

主机测试的 esp_peer_open 是边界桩，不能用桩中的 codec 检查冒充真实底层复现；结论同时依据真实接口要求、生产转换源码与上述真机前后记录。

## f7b4279：应替换，不把 API 缺口当作不可替代根因

已确认的功能缺陷是：Owner 改变策略后，LiveKit 服务端权限更新了，正在运行的设备仍持有旧 Owner 配置和媒体图。重新拉取配置/重连能恢复。这证明业务配置失效处理缺失，不证明必须公开 LiveKit 权限字段。

此前方案的确有效：在 BOX-3 禁麦/恢复测试中先关媒体，再读认证 Owner 配置并重建，6 次正式策略更新均恢复 READY。LiveKit 的 can_publish 从未被用来直接授权麦克风。但“方案有效”和“SDK 扩展必要”是两件事。

两条可替代路径已存在：

1. 官方 on_participant_info 仍收到本地更新；应用已知配置中的本地 identity，能用现有字段识别本机，把更新作为配置失效提示，再拉取认证配置。它不是精确的权限变化事件，实施时必须比较配置、合并提示、处理首次加入和房间 generation，避免无变化也重连的循环。
2. 项目已有 config.refresh 协议及控制器 HandleConfigRefreshCommand；mobile 也有这个操作。可以由共同 Owner/Channel 配置变更通路投递失效通知。当前 Hub output_policy 的 on_changed 接的是 channel_binding.execute，后者刷新 Provider 绑定；该链条没有建立设备已收到并采纳新配置的保证。不能只在管理界面补一次发送就宣称可靠同步已经解决。

此次 check_upstream_callback.py 从上述官方不可变提交提取真实公共结构与转换函数，编译后用 ASan/UBSan 验证：加入、撤销发布、恢复发布的三个输入都会送达现有公共回调，identity/sid 保留，可识别本地。结果 PASS。此实验只验证转换边界，输入是构造的 protobuf 信息；它不冒充新的服务器或硬件实验。真实 ParticipantUpdate 投递已由既有权限测试和 engine 源码链条支持。

新增加的三个布尔值还不是完整配置版本：Provider 用 microphone/camera 生成 can_publish_sources，can_publish 仅是 sources 是否为空。摄像头保持开启时，关麦克风仍可保持 can_publish=true；当前 observer 不观察 sources，就看不到这种变化。这是现有 Provider 逻辑允许的反例，不代表当前四台无摄像头音频板已经发生过该问题。输出策略也有不改变三个传输布尔值的变化。因此不能把这套 observer 当成所有 Owner 配置变更的共同机制。

撤销迁移必须同时替换应用对新增字段的引用，并保留已完成的安全关闭、认证配置读取、原有退避、会话连续性和共享访问边界。先降 SDK pin 会编译失败；只删除 observer 则恢复“运行中开麦不生效”的功能缺陷。

迁移验收需要覆盖：首次加入无刷新循环、重复/远端更新不重建、认证拉取失败不打开媒体、快速策略变化最终采纳最新配置、断线后取最新配置，以及真机禁麦/恢复和对话中切换。主机边界实验不足以直接替代这些验收。

## 本轮实际状态

- 修正了旧报告对权限问题的根因表述，保留旧 HIL 记录及其边界。
- 新增并执行官方回调转换边界实验，PASS，ASan/UBSan 无报告。
- SDK 三个提交、应用生产代码、SDK pin 和四台设备固件均未回退；f7b4279 仍是本地未发布提交。此文是评估结果，不是已完成生产迁移的声明。
- TCP 握手前无 SYN-ACK 到达 lwIP 的无线故障不在这三个修改的作用范围内，其具体根因仍未闭环；不得拿 SDK 修改来解释或掩盖该故障。
