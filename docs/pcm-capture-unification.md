# 共享 PCM 采集机制与 BOX3 分支审计

日期：2026-09-21。基于 `f2ba14a8` 的源码、提交历史、本机 ESP-IDF 5.5.4，以及当天 StackChan 实测。此次没有重新烧录或测试 BOX3。

## 结论

PCM 帧边界、溢出处理、读写、时间戳和生命周期应是所有板子共用的协议。BOX3 没有需要独立 PCM 队列算法的证据。当前源码也只有创建、销毁和分配失败日志按 BOX3 分支，Push/ReadFrame 已共享。

BOX3 的内部 SRAM 分支是历史回退措施，不是已证明的硬件限制。旧实验同时改变了内存位置和队列可用容量，不能证明 PSRAM 本身导致噪声。修复采样边界后有理由重新测试并尝试取消该例外，但 StackChan 的实测不能替代 BOX3 在 AEC/全双工负载下的验证。

## 共享链路及应保留的差异

```text
板级 codec / I2S（真实采样率、麦克风与参考声道）
  → EidolonAudioInput（转为 16 kHz）
  → EidolonMicCapture
      AEC 能力已验证：交错 mic+reference → AfeAudioProcessor → mono
      无设备 AEC：ReadMonoPcm16k → mono
  → PcmPushCaptureSource（16 kHz、mono、int16）
  → livekit_board 的采集门控 → esp_capture / 编码发布
```

依据：`main/eidolon/audio/eidolon_mic_capture.{cc,h}`、`main/eidolon/livekit_board.cc`、`main/eidolon/eidolon_device_profile.h`。

| 层级 | 统一范围 | 保留的配置差异 |
| --- | --- | --- |
| codec / I2S | 相同接口和错误语义 | 引脚、芯片、输入采样率、声道映射 |
| 输入处理 | 统一输出格式 | 按已验证的 AEC 能力选择 AFE 或 raw；不按板名复制队列 |
| PCM 队列 | 完整采样读写、溢出丢整采样、启动清空、停止唤醒、PTS | 容量与内存预算可作为策略 |
| 交互门控 | 共用状态与门控机制 | 全双工、半双工、PTT，由交互策略决定 |

BOX3 当前 profile 启用设备 AEC，codec 输入 24 kHz、参考声道开启；独立 AEC 文档有 refch1/其他参考声道对照及回声消除指标。这些是与 PCM 内存位置不同的证据，不能因为统一队列就移除。参见 [BOX3 AEC 资格记录](aec_qualification/esp-box-3.md)。当前构建脚本选择 low-cost AFE；历史资格记录不等于当前所有配置已经重测。

## 特殊分支来源及证据强度

| 提交 | 变化与意义 |
| --- | --- |
| `b08bbb8e`，7/20 | 为节省内部 RAM 将共享 ring 改为 WithCaps/PSRAM；Push 没有完整采样截断 |
| `01279ea4`，7/24 | 回退普通动态分配。提交记录称 BOX3 146.371 秒服务端 PCM 清晰、STT 和播放中打断正常；这证明回退组合有效，不证明仅内存位置有因果关系 |
| `08857b3c`，8/25 | 因内部最大连续块不足导致 LiveKit engine 创建失败，再次将共享 ring 放 PSRAM；解决的是内部内存预算问题 |
| `192b3453`，8/27 | 仅 BOX3 恢复普通动态分配，同时新增 `!running_` 禁止启动前写入。分配 API、容量、启动前生产行为一起变化，归因未隔离 |
| `f2ba14a8`，9/21 | 所有板子的 Push 统一按 int16 边界截断，ReadFrame 拒绝奇数字节请求；StackChan 实机证明修复了持续字节错位 |

本次核对的 IDF 5.5.4：

- `components/freertos/FreeRTOS-Kernel/stream_buffer.c`：普通动态创建额外分配一字节哨兵，请求 N 的可用容量为 N。
- `components/freertos/esp_additions/idf_additions.c`：WithCaps 分配 N 字节后直接调用静态创建，可用容量为 N−1。
- 请求 16000 时两条路径分别可容纳 16000 / 15999 字节；旧 Push 允许非阻塞部分写入，因此能写入半个 int16。
- 新 Push 将本次可写字节数向下对齐到 2 字节。它依赖单生产者、单消费者；消费者只增加可用空间。

这构成对旧 BOX3 现象的具体替代解释，尚不是其历史根因的实机追认。没有找到旧实验在相同有效容量、相同启动门控、相同边界保护条件下只比较内存位置的证据。不能再把旧注释“PSRAM causes corrupted/noisy capture”作为硬件约束引用。

## 初版方案与复审结论

**初版方案不能直接作为完整实现方案通过评审。** 它解释了容量和内存位置，却把线程退出、失败传播和 SDK 消费者契约放在“相邻机制”里。实际上这些决定队列能否安全销毁，必须纳入本次统一的完成条件。以下复审结论取代初版仅统一分配器的范围。

### 复用现有组件的选择

本次检查的是当前锁定依赖的本地源码，不推断其他 SDK 版本已修复。`build/compile_commands.json` 确认当前构建包含 `gmf_audio_src.c`。

| 选项 | 决策和依据 |
| --- | --- |
| 继续使用 FreeRTOS stream buffer | 采用。现有需求是一个生产者、一个消费者、不同粒度 PCM 的连续字节流；SDK 已提供回绕、阻塞和同步。应用只负责 PCM 格式、所有权及生命周期，不再实现环形队列算法 |
| `xStreamBufferCreateStatic` + 显式存储所有权 | 采用作为统一分配入口。这是 SDK 公共 API；明确 N+1 存储和释放责任是薄适配，不是自研 allocator。控制块内部内存，数据存储按资源配置；不复制 SDK 私有结构或 DeleteWithCaps 实现 |
| FreeRTOS message buffer / ESP-IDF item ring buffer | 不采用。当前需要将 AFE 输出重新分帧供编码器拉取；按消息保存仍需额外拆包、剩余片段所有权和回收管理，不能自然解决停止确认问题 |
| ESP-IDF byte ring buffer | 不采用。也可承载 PCM，但读侧需 acquire/return 管理；替换并不自动提供本会话的退出协议，没有足够收益证明需要迁移 |
| esp_capture 内部 `data_queue` | 不采用。当前入口位于 `private_inc`，属于依赖内部接口；不复制或绑定其私有实现来省掉适配层 |
| `esp_capture_new_audio_dev_src` | 不能直接替换整个管线。它直接拥有 codec open/read，而现有 AudioInput 负责输入访问与采样率归一，AFE 又是独立生产者；只替换 raw 分支会产生两套生命周期和格式转换路径 |
| SDK 默认 AEC source | 不直接替换。当前实现使用 `AFE_TYPE_SR`，不是本项目已验证的语音通话 AFE 路径。统一 PCM 不应顺带更换声学算法 |

### 必须一并解决的评审发现

这些是源码确认的缺口或条件性风险，不是把所有历史音频/重连现象归为同一个根因。

| 优先级 | 发现 | 统一实现的要求 |
| --- | --- | --- |
| P1 | `EidolonMicCapture::Stop` 等待最多约 1 秒后不检查 reader 是否退出；AFE 析构等待 DONE 最多 2 秒后也不检查结果就释放 | 用 RTOS 退出确认闭合所有权；超时返回失败，不销毁仍可能被访问的资源，不允许开始新会话覆盖旧状态 |
| P1 | GMF `audio_src_el_close` 等待 worker 1000 ms 后不判断退出结果就释放 data queue/event，且 source stop 在释放之后调用 | 消费者所属组件必须负责取消、确认 worker 不再访问资源、再释放；仅知道应用 ReadFrame 已返回仍不足以证明 worker 已退出 |
| P1 | `ReadFrame` 停止中返回短帧/空帧却标记 OK；Quiesce 把控制用静音采样塞进音频队列 | OK 必须等于完整请求帧；取消/故障返回错误、丢弃未完成帧。控制状态不进入 PCM 数据流 |
| P1 | WithCaps 删除路径在当前 SDK 中调用错误对象类型的删除 API | 走 SDK 公共静态创建/删除 + 明确存储所有权，且仅在所有使用者退出后释放；真实 IDF 上验证析构 |
| P2 | GMF open 忽略 source start 返回值；应用创建失败不能可靠阻止 worker 启动 | 初始化作为事务：每一步失败都传播且回收已成功部分；检查依赖 start 的错误处理，不能靠稍后 read 失败代替 |
| P2 | GMF 在 ReadFrame 返回后覆盖 frame PTS，并按固定帧数推进 | 明确本项目使用编码发布的连续样本时钟；不能声称仅改 source PTS 就修好了溢出的时间轴 |
| P2 | `Flush` 忽略 reset 失败；`volatile` 和 task handle 轮询没有建立完整退出同步 | reset 仅在无在途读写的停止态执行并检查结果；状态可见性用原子变量，退出所有权用 RTOS 确认，二者不能互相替代 |

源码定位：`main/eidolon/audio/eidolon_mic_capture.cc:100`、`main/audio/processors/afe_audio_processor.cc:86`、`main/eidolon/audio/pcm_push_capture_source.cc:96` 和 `:149`、`managed_components/espressif__esp_capture/impl/capture_gmf_path/src/elements/gmf_audio_src.c` 的 open/read/close。

### 所有权与生命周期

保留现有分层，不新增通用 audio framework、另一套 codec 抽象或后台守护线程：

- `EidolonMicCapture` 负责麦克风 reader 和 AFE 生产者；控制入口串行执行开始/停止。
- `PcmPushCaptureSource` 只负责固定 PCM 格式的生产/消费适配和队列存储，不管理板型或网络重试。
- esp_capture/GMF 负责其 fetch worker 与下游队列，必须在组件内部保证 join 后释放。
- `livekit_board` 串联这些资源的建立/关闭，并向现有会话控制器传播失败；继续保留已有门控，不再额外建一套会话状态机。

生命周期契约是 `Stopped → Running → Stopping → Stopped`；初始化失败回到已完整回收的 Stopped，退出超时进入不可重入的失败状态。内部必要状态不能用更多互不约束的布尔标志替代。

停止顺序：关闭写入准入并请求源取消；通知 reader/AFE 停止；消费者在有界等待内观察取消，ReadFrame 不发布残帧；生产者和 SDK worker 各自由其所有者确认退出；最后 reset 或释放。关闭期间不得 Start、Flush 或析构共享存储。

不再用伪造 PCM 或随意增加延时唤醒消费者。stream-buffer receive 使用有界超时并检查取消，取消响应上界应纳入调用方超时预算；如要即时唤醒，必须用不冲突的 SDK 控制机制并验证其通知槽语义，不能抢占 stream buffer 内部 task notification。读源持续失联要作为采集故障上报，不能无限等待或自动填静音假装成功；正常运行的短暂缺帧和停止取消必须区分。

退出超时不是“继续 free”或“开始下一次重试”的许可。应返回明确错误并保留仍被任务引用的资源，由既有设备故障恢复流程处理；必须有可验证的最终恢复策略，不能让对象永久泄漏后继续创建新会话。析构之前必须已经完成可返回错误的显式关闭，不能让析构承担可能失败的 join。

### SDK 集成边界

GMF worker 的退出确认无法通过应用私下读取其私有 event/queue 修好。落地时先核验是否已有兼容且修复此契约的依赖版本；若采用则显式升级、锁定并回归。若没有，应在依赖所属组件维护可复现、可审查的修复并推动上游，而不是直接修改会被重装覆盖的 `managed_components`，也不是在应用侧增加 sleep 或泄漏规避。这一项未解决，方案不能标记为完整统一完成。

### PCM 与资源契约

1. 对外容量明确表示可用 PCM 字节数，要求至少一个采样且为偶数；检查加哨兵字节的整数溢出。
2. 统一用显式管理的静态 stream-buffer 创建路径：有效容量 N，存储 N+1；控制块明确放内部内存，PCM 存储由配置的内存策略决定。构造失败完整回收，析构调用正确的 stream-buffer 删除函数后释放自有存储。
3. 始终保留完整采样的 Push/ReadFrame 保护。容量对齐不能替代边界保护。
4. 内存选择与音频算法分离。资格验证阶段允许 BOX3/internal 与其他板/PSRAM 配置不同，但两者走同一创建、读写、销毁逻辑；不要仅把现有 `#if BOX3` 搬到另一文件就声称已经完成统一。
5. 不做隐式 PSRAM→内部 RAM 回退：之前确实有内部最大连续块不足导致入会失败的记录。无 PSRAM 的目标需要显式预算，不应让回退重新引入 engine 分配失败。
6. 记录请求/实际可用容量、内存策略及溢出量，使语音故障能够与队列拥塞、网络丢包区分。

7. 保留当前非阻塞、溢出丢弃本次尾部完整采样的策略；不在此重构中偷偷改成覆盖最旧样本、塞静音或跳跃 PTS。16000 字节在 16 kHz mono int16 下相当于 500 ms 缓冲，是延迟预算而非随意内存数值；需要单独测量积压延迟后再调整。
8. 接口固定使用 16 kHz mono int16，与已有上游归一一致；不要为了“通用”增加任意格式和多生产者支持。校验空指针、无效/超大容量、采样数转字节溢出、格式协商和完整帧长度。
9. 内存资源策略放进现有构建/profile 配置，不能按 AEC 开关推导“必须 SRAM”。两个实现候选使用相同有效容量和控制块位置；硬件测试完成前保留 BOX3 当前已验证默认值，测试通过后删除迁移例外而不是长期保留隐藏兼容分支。

以上为复审后的设计约束，**本轮未实施新的分配器或改变 BOX3 的内存策略**。方案评审与实现/硬件验收分开记录，不以“设计已写完”替代依赖问题闭合。

## 合并分支之前需要覆盖的相邻机制

- **析构配对**：本机 IDF 5.5.4 的 `vStreamBufferGenericDeleteWithCaps` 在 stream-buffer 分支调用了 `vSemaphoreDelete`。当天临时 StackChan 探针在销毁时曾触发堆错误，生产对象长期驻留使它不易暴露。这是独立于采样错位的问题；不能用当前主机 stub 的析构成功来证明真实 SDK 析构安全。统一所有板都调用 WithCaps/DeleteWithCaps 不是合格方案。
- **停止与重启**：当前调用链先停止 mic/AFE 生产者，再 Quiesce 唤醒消费者，之后关闭 esp_capture。应验证生产者确实退出、消费者不再读写后才 reset/free；`volatile` 本身不是跨核同步协议。
- **Flush**：当前 `Flush` 忽略 `xStreamBufferReset` 返回值，SDK 在存在阻塞读写任务时可拒绝 reset。现有代码只在 Start 内调用 Flush；未来不能把它当作可随时安全调用的公共清空方法。生命周期加固属于独立验证项，当前没有据此认定另一个已发生的故障。
- **测试边界**：现有主机测试模型覆盖容量差异和采样完整性，不覆盖 IDF 堆实现、双核并发、PSRAM/cache 时序或真实 AEC 性能。

## BOX3 受控验证与取消例外的条件

先保留当前 internal 版本作为基线。候选版本使用相同的静态创建路径、相同 16000 字节有效容量、相同控制块位置、相同边界保护与启动门控；只改变 PCM 数据存储的 INTERNAL/PSRAM caps。固定固件基线、SDK、codec、AEC 模式、声道、Wi-Fi/显示负载与声源。

| 验证 | 要回答的问题 |
| --- | --- |
| 已知 PCM 序列，反复填满/溢出/回绕/排空 | 两种内存策略是否都无错位、重排或非预期丢采样 |
| 真实 IDF 构造失败、创建销毁循环 | 是否无堆错误、泄漏，失败是否正确上报 |
| 静音、近讲、设备播放、双讲 | ring 前后 PCM 是否保持正常，AEC ERLE/残留是否退化 |
| 并发 Wi-Fi、屏幕刷新、入会与重连 | 是否出现溢出、内部块不足、采集延迟或卡顿 |
| 连续采集及多轮开始/停止/重连 | 是否有启动旧帧、reset 失败、消费者未退出、堆增长 |
| 最终 STT 和播放中打断 | 用户可见行为是否至少保持当前基线；网络丢包单列，不只看是否出现文字 |

两种存储策略均通过后，BOX3 可切到统一 PSRAM 默认值并删除板名例外。若在等容量、等语义下仍稳定复现仅 PSRAM 失败，再保留显式内存策略，并以新日志/PCM/时序证据解释原因。

## 本轮完成与限制

- 完成共享链路、BOX3 分支提交来源、IDF 容量/析构机制审计；纠正源码中未经隔离实验支持的 PSRAM 因果注释。
- 复跑 `bash tests/run_pcm_capture_source_tests.sh`：BOX3 / 非 BOX3 两种现有分配路径的主机模型均通过 ASan/UBSan 下的溢出采样完整性、奇数请求拒绝及重启 PTS 测试。
- 无功能代码改动、无烧录；没有声称 BOX3 PSRAM 方案已通过硬件验收。
- 独立 `eidolon-client-esp32-korvo-1` checkout 仍有旧 Push 实现，统一主仓库不会自动更新它或已烧录设备。

## 复审后的验收门槛

1. 主机契约测试：真实适配类的边界/回绕/溢出、完整帧、取消残帧、非法参数、重复 stop/start、失败回滚；已有 deque stub 仅证明采样边界，不作为线程/SDK 安全证明。
2. 真实 IDF 测试：两种内存策略、分配与任务创建故障注入、阻塞读时停止、AFE/reader/SDK worker 延迟退出、反复销毁重建；确认超时路径无释放后访问、无错误重入、正常路径无泄漏。
3. 构建与硬件：BOX3 AFE/full-duplex 与 StackChan raw/half-duplex 使用同一提交和依赖锁，保持声学配置；内存、音频质量、延迟、STT、重连后无旧帧及退出协议均合格。
4. 分发：统一变更及相关依赖锁必须进入各实际构建分支；独立 Korvo checkout 先检查分支差异再同步，不复制单文件掩盖版本分叉。

失败判据：任何一项仍依赖板名补丁、固定延时假定退出、忽略 reset/start/join 返回值，或仅凭主机模型宣称硬件通过，都不应通过完整统一评审。
