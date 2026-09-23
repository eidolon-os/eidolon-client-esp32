# BOX3 控制事件积压调查（进行中）

## 已复核的事实

设备运行 cbdeae6b4，LiveKit SDK 8fa3491，内部 RAM PCM 策略；能力策略 revision=6，麦克风和支持的输出全部开启。本次记录对应 2026-09-22 21:53:40–21:54:27 的会话。

串口从设备启动后的 399189 ms 到 431409 ms 记录 265 次入队失败。按该固件生成的 sdkconfig.h（COMPANION_FACE 开启，GUARD_SERVICE 和 OWNER_FACE_PROFILE 关闭）映射：

|编号|事件|次数|含义|
|---|---|---:|---|
|20|AudioTick|188|轮询采集门控、播放状态、音频状态心跳和动作超时|
|19|SessionActivity|54|转写活跃通知，刷新闲置计时；不是转写正文|
|18|AgentPhaseChanged|12|Agent 阶段，影响 UI、采集门控和闲置计时|
|13|PresentationEvent|6|表情呈现生命周期回执|
|11|ControlCommand|5|控制命令；旧日志未保留 op，不能断定都是 playback.stop|

转写正文通过另一条回调交给 Application::Schedule，因此 SessionActivity 丢失不能直接解释为字幕文本丢失。没有证据表明这 265 次记录是 PCM 音频帧丢失。

服务端完成四次 thinking → speaking → listening；设备正常发送 session_close。服务端完成不代表设备端控制可靠性验收通过。

## 已定位的架构和错误传播缺陷

- 单个 24 格 FIFO 同时容纳定时轮询、可合并的活跃通知、阶段变化和不可随意丢失的控制命令。所有生产者均零等待入队，满时一律销毁。
- 80 ms 定时器在消费者落后时持续累计过时的轮询任务；这些任务处理时读取当前状态，不需要保留每个历史 tick。
- JoinRoom、LeaveRoom、SetMicEnabled、PublishDeviceEvent 等接口在入队失败后仍返回 ESP_OK；调用方无法区分“请求已接收”和“请求已丢弃”。
- Handler 在单个控制任务上同步执行，包含网络发送、播放 flush、配置与生命周期操作。必须测量阻塞源，不能仅由队列满推定 CPU 优先级或某个网络调用。
- git blame 显示队列和满队列直接丢弃机制来自 1305c6a24（2026-06-20）；这证明设计早已存在，不足以证明本次触发因素与新修改无关。

## 当前定位手段与边界

增加事件排队/处理耗时、LiveKit 数据发送耗时，以及丢弃命令 op 的观测。未扩大队列、改变优先级、PCM 内存策略或任务归属。待真机复现后决定阻塞源修复；该观测构建不是修复完成的声明。

后续修复必须区分可合并状态与有交付语义的命令，保持单一状态所有者，并向调用方传播接收失败。具体唤醒/背压机制沿用 FreeRTOS 原语，不另造队列算法。尚未完成修复和验收。

## 2026-09-23 真人反馈：两次无回复

11:02–11:04 的服务端记录显示：前三轮正常；11:03:02 与 11:03:14 两轮均收到最终 ASR 转写并进入 thinking，但 Agent 返回 `invalid_presentation: SUCCESS_INTENT_REQUIRES_COMPLETED_OUTCOME`，随后直接回到 listening，没有进入 speaking。后续四轮恢复正常。

代码边界为 Agent `domain/agent/presentation.py::validate_response`：confirm/celebrate 必须引用本轮成功且 outcome_state=completed 的工具结果。`turn.py` 对该校验失败直接结束整轮并发送错误。因此这里是模型终态候选与事实约束不符、错误被整轮传播的故障，不能把这两次静默当作设备队列积压的证据。尚未修改该约束或发布服务端修复。

设备串口原采集窗口在此次测试前已结束，缺少对应设备耗时证据。已于 11:06 重新开启无复位串口采集，窗口延长为 4 小时，等待下一次复现。不能用缺失日志证明本轮没有设备队列丢失。

本地已修正 Enqueue 返回错误，并让 JoinRoom/LeaveRoom/SetMicEnabled/PublishDeviceEvent 传播接收失败；Transport 对加入/退出拒绝记录错误，麦克风请求拒绝时不更新持久化设置。BOX3 编译通过，尚未烧录，仍非整个拥塞问题的完成修复。

## 2026-09-23 11:27–11:30 连续对话验收

用户反馈多轮均有回复。服务端记录 7 次完整 thinking → speaking → listening，无 invalid_presentation/session error，最后 USER_INITIATED 关闭。Agent 7 轮生成耗时约 1.09–1.45 秒，本轮未出现附加表达降级日志；降级分支另由回归和目标机安装包故障输入验证。

设备侧本轮收集完整：77 条转写流关闭记录，零 Event queue full，零 dropped control，普通对话事件没有 >=80 ms 的处理耗时记录。Join 事件 4010 ms、Leave 81 ms，均 pending=0；发布时的 Reconnect 1795 ms 也 pending=0。因此不能把入会刷新耗时认定为原来的在会积压根因。

服务端音频状态观测对比：故障轮 37 条，序号 21→22 的接收间隔为 6.116 秒；本轮 260 条，最大相邻间隔 0.636 秒。旧队列满同时伴随设备状态上报显著滞后，这是可验证的影响；接收间隔本身不独立证明停在某一函数。旧 5 条控制命令的具体 op 未记录，不能事后精确认定哪些是 playback.stop。

结论：本轮回复正常，队列异常未复现。共享 FIFO 无合并/无关键命令交付保障和失败伪装成功是确定的设计缺陷；导致那次消费者落后的具体调用/调度条件仍未闭合。不得把新增观测、正常一轮或 Agent 呈现隔离修复当成队列根因已解决。
