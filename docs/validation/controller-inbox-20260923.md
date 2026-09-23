# 控制队列：可控复现、原语选择与验收边界

本轮接续 `c6994a90`，在 `audio-capture-unification` 工作树实施；共用 SDK 未修改。

## 证据链

原实机故障和七轮正常对话见 [原始调查](controller-event-overflow-20260922.md)。
**原始消费者停顿的直接触发原因仍未知。** 265 次丢弃不等于 PCM 丢帧；旧日志没有 control op，不能倒推出五条命令的内容。

源码证实 AudioTick 每 80 ms 投递，handler 只采样当前状态；SessionActivity 只重新设置闲置兜底计时器，不承载转写正文。原 FIFO 的 24 格容量可被 24 个历史 tick 占满，停顿超过约 1.92 秒便失去控制命令接收空间（实际门槛还受原有占用和定时器相位影响）。恢复后补做过时 tick 并不能重建历史音频状态。

新增真实内核反例：ESP-IDF 5.5.4、Espressif QEMU ESP32 双核，真实 `ESP_TIMER_TASK` 每 80 ms 发一次，消费者暂停 30 次触发。旧队列得到 24 个 tick、拒绝 6 个 tick，随后控制命令也被拒绝。相同定时器通知新 inbox 后，FIFO 占用为零，全部 24 个控制事件可以接收。这证明**可合并维护请求挤占控制队列**的缺陷与修正，不证明实际故障当时是哪个 handler 停顿。

## 官方机制比较

| 机制 | 语义及本项目判断 |
| --- | --- |
| FreeRTOS FIFO | 保留事件顺序及载荷。零等待且已满会立即失败；继续用于命令、阶段变化、生命周期和呈现回执。接收成功不等于执行成功。 |
| Task notification / `eSetBits` | 同一位的重复通知合并，适合单消费者“需要重读当前状态”。本实现用两个位保存 tick/activity 请求，另一个位唤醒 FIFO 消费者；不存放命令。 |
| Event Group | 同样支持合并位，适合多等待者；这里只有一个消费者，任务通知即可。若 controller handler 将来需要占用通知 index 0，必须改用专用索引或独立 Event Group，不能共享消费同一通知。 |
| Queue Set | 等待多个队列/信号量，不会自动合并历史 tick，也不处理 handler 阻塞。当前没有采用的必要。 |
| `esp_event` | 自带有界队列、串行 handler 和投递超时；换成它并不会自动消除当前 FIFO 饱和问题，还需处理现有载荷所有权。 |
| `esp_timer.skip_unhandled_events` | 处理定时器自身延迟造成的到期堆积，不能合并已经由回调投进应用 FIFO 的事件。回调仍只做短小通知，不阻塞等队列空间。 |

依据：[IDF 5.5.4 FreeRTOS API](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/freertos_idf.html)、[FreeRTOS 通知位](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/03-Direct-to-task-notifications/04-As-event-group)、[IDF esp_event](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/esp_event.html)、[ESP Timer](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/esp_timer.html)。也核对了本机 5.5.4 的 `queue.h`、`task.h` 和 `esp_timer.c`，没有假定新版本行为。

## 实施语义

`ControllerEventInbox<Event>` 只组合原生 Queue 和 Task Notification，未实现另一套队列算法。所有板子共用它；FIFO 仍为 24，控制任务优先级仍为 5，未改变 PCM 内存策略。

- AudioTick、SessionActivity 不再占 FIFO，重复通知合并成至少一次后续采样/活跃处理。它们不再保留与命令之间的历史 FIFO 顺序；handler 仍检查当前会话状态，命令和阶段边沿本身不合并。
- 每次循环最多取一个有序事件、处理各一份维护通知；即使 FIFO 一直有数据，也检查维护位。维护洪峰也不会无限先于 FIFO 执行。
- 先投递 FIFO 再通知。消费端有积压时不休眠，空检查之后到达的生产者会留下待处理通知，避免漏唤醒。
- 通知在 wait 返回时原子清除；handler 运行时新到达的位留给下一轮。禁止其他同步调用借用 controller 的通知 index 0。
- 载荷仍由原 Enqueue/Dispatch 清理；任务创建失败、队列满仍向现有可返回错误的接口传播失败。网络命令原有 ack 协议没有被替换为第二套重试协议。
- dispatch 观测同时检查排队和执行是否达到 80 ms。合并通知不保存单次入队时刻，`wait_ms=-1` 明确表示不可测，不能解释为零等待。

此改动不保证任意命令洪峰都能接收：真正的命令队列满仍拒绝。也不解除在执行 handler 的网络、显示或播放等待。单一状态所有者不等于所有外部副作用都已有时间上界。

## 阻塞路径审计

| 路径 | 查到的事实与边界 |
| --- | --- |
| AgentPhase → UI | Application 回调使用 Schedule；主任务移出回调列表后释放 mutex 再执行 UI。不能直接宣称这是“主任务持队列锁导致控制器等待显示”。 |
| AudioTick → capture gate / RMS | 当前 BOX3 的 gate 更新和 RMS 读取不等待 PCM；Board::SetCaptureQuiet 使用空基类实现。尚不构成原故障的耗时证明。 |
| PublishData | LiveKit `livekit_room_publish_data → engine_send_data_packet → peer_send_data_packet → esp_peer_send_data` 同步调用；已加发送耗时观测。不能仅凭服务端两包接收间隔归因到此路径。 |
| playback.stop | 直接调用 `av_render_flush`，需要实测；旧日志某一次 flush 返回不代表所有丢命令都属于 stop。 |
| expression / 生命周期 / config | 仍有显示及配置等同步工作，需在真正故障窗口按 dispatch 类型和子调用耗时定位。 |

LiveKit 调用链以本机锁定 SDK `8fa3491` 为准；另查阅 [LiveKit 官方源码](https://github.com/livekit/client-sdk-esp32/blob/main/components/livekit/core/engine.c) 和 [ESP Peer 1.4.2 官方说明](https://components.espressif.com/components/espressif/esp_peer/versions/1.4.2/readme)。后者没有提供足以推出该次发送等待时长的保证；未把 upstream main 当作当前安装版本。

## 自动化矩阵

运行入口：`bash tests/run_controller_inbox_tests.sh`，见 [测试说明](../../tests/controller_inbox_idf/README.md)。编译的是生产 inbox 头文件和真实 IDF 内核，无仿造 Queue/Notification 实现。

| 测试 | 结果 |
| --- | --- |
| 原 24 格 FIFO 反例 | 确定复现随后控制事件拒绝 |
| 10,000 次合并通知 | FIFO 占用为零；24 命令容量完整 |
| 真实 80 ms timer + 2.4 s 慢消费者 | 旧丢 6 ticks + 拒绝控制；新 24/24 控制接收 |
| 25th 命令、FIFO 顺序、拒绝后恢复 | 通过；不把拒绝当成功 |
| handler 期间再次通知 | 通过；下一轮仍可见 |
| 两生产者共 4,000 次尝试 + 100 ms 停顿 | 通过；示例运行接收 233、拒绝 3767，已接收全部处理，无同源乱序；维护处理 210/210 次。计数随调度变化。 |
| 1,000 次空队列/休眠交界投递 | 通过，40 秒上限内全部处理 |
| BOX3 / StackChan 生产构建 | 完整构建通过；未更改依赖锁与共享 SDK |
| pending session control / control output deadline / expression delivery / controller worker resources 回归 | 全部通过 |
| controller 端到端生命周期、拒绝回执、真实网络故障 | 未由本 inbox 测试覆盖 |
| 原实机停顿及修复后压力/声学表现 | 未由 QEMU 测试证明 |

测试日志仅存 `.cache/controller-inbox-*.log`。设备私有证据仍在 `.cache/input-policy-20260922`，不提交身份、凭据或网络内容。

## BOX3 安装记录

已核对目标 MAC、实际分区表、旧镜像 SHA-256 与原始完整 16 MiB 备份。只替换 ota_1，原 ota_0、NVS、owner_trust、otadata 写前写后全部读出逐字节一致。新镜像完整分区读回的应用前缀与构建二进制逐字节一致。

构建基于 c6994a90 加本报告对应源码改动（运行 build stamp 带 dirty）；SDK 8fa3491，3470064 bytes，SHA-256 `09d0f7d90697128a1c0cf1677eaf06d7e767b8738a95d8e6014672d8a11d9324`。私有 `controller-inbox-source.json` 记录三个生产源码文件的哈希。StackChan 仅构建，未烧录。

实机启动：约 12.139 秒 `operational_ready=1 / READY`，无队列溢出、断言、Guru Meditation 或任务看门狗告警。初始化事件 715 ms、连接状态事件 111 ms，均 pending=0；不作为原在会阻塞的复现证据。启动验收只证明新固件可以恢复控制通道，尚不替代多轮语音、声学打断、并发生命周期及慢网络验收。

剩余闭环：取得在会慢 dispatch 的真实类型与 PublishData 等子调用耗时，再针对该路径做有界故障注入与恢复验证。网络命令饱和时的端到端拒绝/超时处理，以及 PTT/生命周期事件在纯命令洪峰下的交付语义，也尚未由此次 inbox 测试证明。不能将这些未覆盖项表述为已解决。
