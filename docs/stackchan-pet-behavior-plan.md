# StackChan 桌面宠物化：动作 / 表情 / 触摸反馈整体方案

状态：方案草案（2026-09-21），2026-10-31 按项目更新复核（变更见 §9），未开始实现。

## 0. 一句话结论

现在的 StackChan 是「一台会转头的对话终端」，不是宠物：头部只有 6 个离散手势、脸只有 8 个手势 + 4 个基态、没有任何空闲自主行为、没有嘴型同步、机身上的三区触摸板 / IMU / 接近光传感器 / 12 颗 RGB 全部闲置或只在「唤醒」时亮一下。要变成宠物，缺的不是「再加几个手势」，而是三层东西：**一个常驻的活性层（idle liveness）、一个本地反射层（touch / IMU 反射）、一套多轨动画数据格式（头 + 脸 + 灯 + 声 同步）**，再加一条把这些接进 agent 语义层的契约通路。

2026-10-31 复核后，约束重新定性。StackChan 关闭了唤醒词，靠屏幕点按开始对话，**待机时 mic 是关的**。待机是设备大部分时间，所以空闲活性和触摸反射在待机没有声学冲突，不需要先做声学实验。真正挡路的是一个现有缺陷和一个权限空白：对话结束后舵机电轨不恢复（§4.1）；输出门是会话作用域，待机时一律拒绝动作、表情和音效（§4.5.1）。语义层也已经变了：服务端在 10 月落地了 `stackchan.head.v1`，原方案的 `eidolon.body.v1` 设想作废，改为在它上面扩展（§1.5、§4.5）。

---

## 1. 现状盘点（含代码位置）

### 1.1 舵机 / 身体

- 硬件：Feetech SCS0009 串行舵机 ×2（yaw id1 / pitch id2），UART1 1Mbaud；软件限位 yaw ±128°、pitch 3°…87°（`main/boards/m5stack-stackchan/config.h:93-96`）。弹簧阻尼插值，50 Hz tick（`servo.cc:133-150`、`stackchan_body.cc:117-150`）。
- 手势表只有 6 个，全部硬编码在 `RunGesture()`（`stackchan_body.cc:335-372`）：`nod / shake / perk_up / droop / glance / wake_wobble`。每个都是「几个固定角度 + 固定 hold」，无缓动曲线、无随机、无幅度参数。
- **没有任何空闲行为**：无呼吸、无随机看、无眨眼配合的转头。唯一的「空闲」是舵机自动卸力（`servo.cc:65-72`）。
- 舵机电源门控：`SetCaptureQuiet(true)` 直接切断 VM EN 并冻结 motion；`PublishClientAudioState` 在半双工空闲时通常打开 mic，但 `head_deadline_us_` 有效时会令 `capture_on=false`，现有受控动作可以运行。新增活性层必须复用采集/运动协调，不能另开一路舵机电源控制。
- 安全护栏已完备：`safety.stop`、5 s 运动 deadline、TTL、pitch 堵转保护、`BUSY` 互斥。这一层不需要动，新方案全部在它下面跑。

### 1.2 表情 / 脸

- 渲染：LVGL + 移植的 M5 avatar 皮肤（`main/eidolon/avatar/skins/default/`），`CompanionFaceView` 驱动。
- 表情词汇 = SDK 契约 `face-core-1` 的 8 个 gesture（`attend/affirm/ponder/question/soften/delight/hesitate/attention`，`expression/generated/presentation_catalog.h:16`）+ 4 个基态（`Idle/Listening/Thinking/Error`，`expression/core/runtime.h:10`）。每个 gesture 是一个静态 `FacePose`（`runtime.cc:15-28`），在 plan 的 step 上做包络。
- 眨眼有（4.3 s 周期），**嘴不动**：`mouth_open` 只在 `Delight/Attention` 两个 pose 里是非零，`AgentSpeaking` 阶段没有任何东西驱动嘴（`main/eidolon/views/companion_face_view.cc:471`）。
- 已经移植但**一行都没被调用**的资产：`Emotion {Neutral, Happy, Angry, Sad, Doubt, Sleepy}`（`avatar/avatar/elements/emotion.h`）和 5 个 decorator（`angry / heart / shy / dizzy / sweat`，`avatar/decorators/`）。
- 本地反射 `AvatarExpress()` 只认 3 个字串（happy/thinking/sad），且 `body.presence.set` 的 legacy 分支传 `"sleepy"` 会被静默丢掉（`companion_face_view.cc:428-431`）。

### 1.3 触摸 / 传感器（机身）

- 出厂硬件（M5 官方文档）：**Si12T 三区电容触摸板（I2C 0x68，头顶）**、BMI270 六轴 + BMM150 磁力计、LTR-553 接近/光感、IR 收发（G5/G10）、ST25R3916 NFC（0x50）、INA226 电量（0x41）。
- 本仓库：**以上全部未接**。grep 无 `0x68 / bmi270 / ltr553 / si12t`。唯一的「触摸」是屏幕 FT6336，Hub 模式下只作 LVGL 指针（`m5stack_stackchan.cc:236-260`）。
- 出厂固件 `firmware/main/hal/hal_head_touch.cpp` 的读法已查清（见 §3.3），可直接移植。

### 1.4 声音 / 灯

- 本地音效只有 3 个合成音（identify 双音、startup chime、roll call），全部在 `eidolon_local_feedback.cc`，无 wav 资产。
- RGB 只有一个效果 `RgbMarquee`（青色跑马灯），只在 `body.presence.set awake` 时触发一次；agent 说话 / 听 / 想的时候灯全黑。出厂固件是「绿=听、蓝=说、灭=闲」。

### 1.5 语义层（服务端）：2026-10 已落地两条头部通路

- **隐式通道**：channel 每轮回应时把 intent 映射成一个头部手势，与面部 plan 并行下发（`eidolon_channel/eidolon/livekit/agent/session/presentation.py` 的 `present_motion`）。映射是 acknowledge/notify→perk_up、confirm→nod、consider/clarify→glance、comfort→droop、celebrate→wake_wobble、decline→shake。参数固定为 `times=1, hold 350, return 350`，不看 stance、intensity、pace。
- **显式通道**：agent 工具 `stackchan_head`（SDK `eidolon_sdk/biz/presentation/motion.py`，profile `stackchan.head.v1`）。动作有 nod、shake（1–3 次）、look_left/right/up/down、home、stop。用户明确说「点头」「往左看」时调用，回执区分 `software_sequence` 与物理到位。显式动作所在的那一轮会压掉隐式手势。
- **门控**：设备 manifest 声明 `motion.profile=stackchan.head.v1`。会话计划带 `motion_profile`，设备只接受这一个精确值（`main/eidolon/output_policy.cc` 的 `ParseSessionOutputPlan`）。工具描述写明「没有灯光动作」。
- **结论**：原方案设想的 `eidolon.body.v1`、`animation.play`、`perform` 已被这套落地的标准取代，后续扩展在它上面做（§4.5）。

---

## 2. 参考对象与可借鉴点

| 参考 | 借什么 |
|---|---|
| M5Stack 出厂固件（`m5stack/StackChan`） | 空闲每 4–8 s 随机转头 + 眨眼；头顶触摸「竖向滑动→🥰 开心动作」；摇晃→😵‍💫 眩晕；长时间无交互→😴 睡觉；灯色代表状态；舞蹈编排格式（音乐 + 角度序列 + 灯色）。Si12T 读法直接移植。用户 issue #95 提醒：空闲动作耗电，要可关。 |
| stack-chan（meganetaaan） | 「活性」三件套：blink / breath / idle-drift 在 30 FPS 双缓冲上跑；把 face / motion / input / audio 做成 capability API，行为（mod）用数据/脚本描述而不是写死在固件。 |
| Anki Vector / Cozmo | **多轨动画**是核心：一个 animation 同时驱动 head / face / audio / backpack lights，由 emotion engine 按情境挑选。摸头反应参考真实动物（猫头鹰）视频而不是「通用宠物」。眼睛用扫视（saccade）表达「在想」。 |
| Eilik | 按部位区分反应（头/肚/背）；摸多了会烦、被举起来会怕、抚摸能安抚；1000+ 反应靠「少量基础动作 × 情绪调制 × 随机」而不是 1000 段手写动画。 |
| Focus Bot（ESP32-C3） | 小内存上的情绪模型样板：10 个情绪状态、5 种触摸模式（单击/双击/长按/连拍/多区）、5 种运动模式（静止/倾斜/摇晃/旋转/跌落）、5 个人格维度持久化。 |

---

## 3. 目标体验（宠物化的具体表现）

### 3.1 「活着」（无人交互时）

- 呼吸：pitch 在 home 附近 ±1.5° 做 0.15–0.25 Hz 正弦，叠加在一切动作之上。
- 视线漂移：脸的 gaze 每 2–5 s 微动一次（saccade，50–120 ms），偶尔跟头一起转。
- 随机看：每 4–12 s 一次小幅转头（yaw ±10–30°、pitch ±8°），弹簧慢速，回不回 home 随机。
- 阶段性小动作：伸懒腰（pitch 缓升到 70° 再落回）、抖一下、歪头。频率由情绪的 arousal 调制。
- 困倦：连续 N 分钟无交互 → 眼皮渐垂（`Sleepy`）、头慢慢低下、偶发「点头惊醒」；再久 → 睡着（眼闭、头低、呼吸变慢、灯极暗呼吸）；有人来（owner presence / 触摸 / 声音）→ 醒来动作。

### 3.2 对话中（把现有阶段变成身体语言）

| 阶段 | 头 | 脸 | 灯 |
|---|---|---|---|
| Listening | 目前断电（见 §4.1）。目标：微微前倾抬头（pitch +5°）并保持 | 眼略大、随声源微动 | 绿色柔和常亮 |
| Thinking | 歪头 + 视线上移 | ponder + 扫视 | 蓝色慢呼吸 |
| Speaking | 随语句节奏小幅点头/摆头（用 TTS PCM 包络驱动） | **嘴型跟随播放 RMS**（现在完全不动） | 蓝色随音量脉动 |
| 回应 intent | 已落地：channel 每轮发一个固定手势（§1.5）。待改进：一 intent 一动作、不看 stance/intensity；目标是 celebrate×playful→bounce、comfort×warm→lean_in 这类参数化（§4.5.2） | 现有 face plan | 与 stance 对应的色相 |

### 3.3 触摸反应（Si12T 三区头顶触摸板）

输入原语（移植出厂 `hal_head_touch.cpp`）：50 ms 轮询，三通道强度加权成位置 −100…100（左/中/右），`Press / Release / SwipeForward / SwipeBackward`（滑动阈值 40）。在其上再识别：`tap`、`double_tap`、`long_press(≥600ms)`、`stroke`（长按且位置来回移动 = 抚摸）、`rapid_taps`（2 s 内 ≥4 次 = 拍打）、`zone`（左/中/右）。

反应表（每条 = 头 + 脸 + 声 + 灯 的一个多轨动画，带随机变体）：

| 触摸 | 头 | 脸 | 声 | 灯 |
|---|---|---|---|---|
| tap（首次） | 快速抬头看向触摸侧（zone→yaw ±20°） | `question`（一眼大一眼小）+ 眨 | 短「唧？」上扬啾声 | 该侧两颗灯闪一下 |
| stroke（抚摸） | 头缓慢朝手的方向蹭（跟随 position → yaw，pitch +6° 抵手）+ 微幅 1 Hz 摆动 | 眼睑半闭→闭合（`soften`→闭眼）、嘴微笑，**heart decorator** | 低频呼噜（80–120 Hz AM 调制的合成音，随抚摸持续渐强） | 暖粉色慢呼吸 |
| 抚摸 >8 s | 头靠得更低，几乎不动 | 完全闭眼、shy decorator | 呼噜变慢 | 更暗 |
| double_tap | 小幅「嗯？」点头两下 | `attention` | 双啾 | 白闪两下 |
| swipe forward/back | 头随滑动方向转过去再回来 | gaze 同向 | 「呜～」滑音 | 灯跑马跟随方向 |
| rapid_taps（拍打） | 后缩（pitch −10° 快）+ 快速摇头 | **angry decorator**、眼变窄 | 「哼！」短促低音 | 红色快闪 |
| 拍打后 30 s 内再摸 | 不理（只转开头） | sweat decorator | 无 | 无 |
| 被摸醒（睡眠中） | 抬头慢、摇一下 | 眼慢慢张开 | 打哈欠（下滑长音） | 由暗渐亮 |

### 3.4 IMU / 接近（第二阶段，硬件已在）

- 摇晃（BMI270 加速度方差）→ 眩晕：头绕小圈、**dizzy decorator**、「哇哇～」。
- 被举起（Z 轴失重 + 姿态倾斜持续）→ 惊吓：头缩、眼大、「呀！」、红灯；放回桌面 → 长舒气、`soften`。
- 倾斜放置 → 头反向补偿保持「水平」（很像活物）。
- LTR-553 接近：手靠近头顶还没碰到 → 先抬眼看（预期动作，是「宠物感」里最便宜最有效的一条）；环境光暗 → 自动进入低亮度 + 更容易困。

### 3.5 指令 / 表演（充分利用舵机）

已落地的 `stackchan_head` 显式工具覆盖 nod、shake、四向看、home、stop。下面的表演类在它上面扩 profile 版本（§4.5.3），不另开 op。候选动作：`dance`（yaw 大幅摆动 + pitch 弹跳 + 灯随节拍，可配本地合成节拍音）、`look_around`（左中右扫视）、`bow`（鞠躬）、`nuzzle`、`stretch`、`shiver`、`peek`（探头）、`sneeze`、`yes/no`（现有 nod/shake 加幅度和次数参数）、`follow_me`（打开视觉跟随：ATK/摄像头 bbox → yaw/pitch，M0 sense.attention 的下游）。

---

## 4. 架构方案

### 4.1 舵机电源：先修现有缺陷，声学实验降为 P1 可选

2026-10-31 复核结论：电源冲突只存在于**对话中的聆听段**，不在待机。

事实：
- StackChan 构建脚本关闭了唤醒词，入口是屏幕点按（`scripts/eidolon/eidolon-m5stack-stackchan.sh:393-399`）。
- 对话结束时 `SuspendConversationAudio` 无条件关闭采集（`main/eidolon/eidolon_voice_controller.cc:4092-4097`）。待机时 mic 关，电轨理论上可以常开。
- 对话中每轮回应现在都带隐式头部手势。`head_deadline_us_` 有效期间采集被关、电轨打开（`eidolon_voice_controller.cc:4197-4202`），所以 agent 说话期间头能动，这部分已被现有协调覆盖。

**缺陷 A**（代码阅读得出，未上机验证）：关闭采集的路径没有同步调用 `SetCaptureQuiet(false)`。半双工里 agent 说完后 mic 重新打开、电轨被切断；随后对话结束，电轨就一直断着，`capture_quiet_` 一直为真。待机期间任何 `HeadGesture` 都会以 `CAPTURING` 被拒（`main/boards/m5stack-stackchan/stackchan_body.cc:294`）。`body.presence.set` 调用时不带 observer，拒绝被吞掉，仍回报 `applied=1`。这正是 `PublishClientAudioState` 里「两个门由同一个 owner 推导」那句注释想防止的分叉。修法：关闭采集的路径与 `PublishClientAudioState` 共用同一个推导，capture 门和 servo 门一起设置。

决策：
1. P0 修缺陷 A。待机期间活性、触摸反射、presence 反射都在电轨常开下运行，没有声学风险。
2. P0 对话聆听段头保持静止，活性只走脸和灯。宠物在听人说话时本来就该安静，这是合理的默认。
3. P0 聆听段发生触摸，只做脸、灯反应，不出声、不动头、不关 mic，避免一次摸头吞掉用户半句话。
4. P1 可选：如果想要聆听段的微动（前倾、呼吸），再做下面的声学实验，用 STT WAV 量化（期望有调制、无 32768 削顶、ZCR 0.05–0.15）。

声学实验清单（仅用于第 4 条）：
- **按阶段而不是按 mic 门控**：只在 `UserSpeaking / Recording` 时断电，测啸叫是否让服务端 VAD 误判。
- **卸力而不断电**：验证「卸力 + 电轨常开」的啸叫在半双工 + 服务端 VAD 下是否可接受。
- **动作窗口调度**：短暂通电、动、断电，适合离散动作，不适合呼吸这类连续动作。
- 硬件层：VM 轨加 LC 滤波或换开关频率，需要问 M5。

### 4.2 分层：三个行为源 + 一个仲裁器

```
                 ┌────────────────────────────────────────────┐
  Hub/agent ───▶ │ Session layer   expression.play / head.gesture / animation.play │  优先级 2
  (有会话授权)   └────────────────────────────────────────────┘
                 ┌────────────────────────────────────────────┐
  Si12T/IMU/ ──▶ │ Reflex layer    触摸/IMU/接近 → 反应表 → 多轨动画            │  优先级 3
  LTR553         └────────────────────────────────────────────┘
                 ┌────────────────────────────────────────────┐
  时间/情绪 ────▶ │ Liveness layer  呼吸、视线漂移、随机看、困倦、睡眠           │  优先级 4（底层持续叠加）
                 └────────────────────────────────────────────┘
                                   │
                          ┌────────▼────────┐
  safety.stop ─────────▶  │  Arbiter (优先级1)│  ── 头轨 → StackChanBody（现有护栏之下）
  capture gate ────────▶  │  + 输出通道可用性 │  ── 脸轨 → expression::Runtime（新增 overlay/mood 输入）
                          │                 │  ── 灯轨 → PY32 LED
                          └─────────────────┘  ── 声轨 → eidolon_local_feedback（合成器）
```

规则：
- 高优先级启动即抢占低优先级的**离散**动作；liveness 的呼吸/漂移是连续层，永远叠加不抢占。
- 每条通道有独立「可用性」：头（电源轨是否可动）、声（`UserSpeaking` 阶段禁播本地音效，否则进 mic）、脸 / 灯（永远可用）。动画在某通道不可用时**降级执行其余通道**，不是整条丢弃。这样即使 §4.1 没解决，摸头至少有脸 + 灯反应。
- Session 层动作结束后 `on_finish=resume_current_base` 回到 liveness 决定的基态（现有 runtime 语义，不变）。
- **与服务端动作的仲裁（2026-10-31 新增）**：`head.gesture` 现在有三个来源，channel 隐式手势、agent 显式工具、`body.presence.set`。当前 `gesture_busy_` 是先到先得，忙就回 `BUSY`。本地 clip 接入后如果沿用这个互斥，服务端命令会被呼吸或蹭头动作拒掉，agent 就会说「动作失败」。规则改为：服务端命令抢占本地 clip，本地 clip 记为取消且不上报；服务端动作在跑时本地 clip 直接让路。
- 服务端动作执行期间暂停呼吸叠加，保证回执 `software_sequence` 对应的就是那段动作。
- `safety.stop` 之后活性层保持挂起，直到会话结束或下一个显式动作。否则 stop 刚让头放松，呼吸层又把它拉起来。
- 触摸采样和 clip 播放都不进控制器事件队列。该队列容量 24，2026-09 出过溢出（`docs/validation/controller-inbox-20260923.md`）。只有需要上行时，才把触摸开始、结束这类边沿事件交给控制器。

### 4.3 多轨动画格式（数据，不是代码）

现有 6 个手势写在 `if/else` 里，扩到 30 个不可维护。定义一份 `AnimationClip`：

```
clip nuzzle {
  head:  [(0, yaw=+zone*20, pitch=51, speed=300), (900, yaw=+zone*24, pitch=53, speed=200), (1800, ... )]
  face:  [(0, soften, i=.6), (600, eyes_closed, i=1)]   // 复用 FacePose 关键帧
  led:   [(0, breathe, rgb=(255,140,170), period=1800)]
  sfx:   [(100, purr, dur=∞, gain_env=ramp)]
  loop:  while_input_held      // 抚摸类：随输入持续
  vary:  {yaw ±15%, hold ±20%} // 随机化避免机械感
}
```

- 头轨编译成对 `SetHeadAngles/LookAtNormalized` 的关键帧序列，由现有 50 Hz `UpdateLoop` 播放（不新增任务，顾及内部 SRAM 紧张）。
- 脸轨走 `expression::Runtime`：需要新增一个「overlay pose」输入（本地反射不需要 session 授权，但和 blink 一样属于设备原生活性，不经 `expression.play` 契约）。
- 现有 6 个手势迁进 clip 表作为第一批数据，行为不变，作为回归基线。
- 出厂固件的舞蹈编排格式（音乐 + 角度 + 灯色）可作为 `dance` 类 clip 的导入来源。

### 4.4 情绪 / 精力模型（Vector-lite）

三个标量，都在 [-1,1] 或 [0,1]，按时间常数衰减回基线：
- `valence`（愉悦）：抚摸 +、拍打 −、owner 出现 +、被 decline 微 −。
- `arousal`（兴奋）：任何交互 +、时间衰减快；决定 liveness 频率和动作幅度。
- `energy`（精力）：随时间和 arousal 消耗，低到阈值进入困倦/睡眠；睡眠回充；可参考 Hub 时间（HUB_MODE 无 wall clock，只能用 Hub 响应 Date 头，见 memory）做昼夜。

情绪只做**调制**（选哪个变体、幅度多大、灯色偏暖偏冷、idle 间隔多长），不直接产生动作，保证行为可预测、可在 host 测试里断言。持久化只存 3 个数 + 最近交互时间戳到 NVS（注意 NVS 满时优雅降级，memory 有教训）。

### 4.5 与语义层的契约：在已落地的 `stackchan.head.v1` 上扩展

#### 4.5.1 待机本地输出的权限（新增，P0 必须先定）

输出门 `DeviceOutputGate` 是会话作用域。对话结束后 `Close()`，之后 `Allows(...)` 一律返回 false（`main/eidolon/output_policy.cc:134-149`）。StackChan 默认要求输出策略（`EIDOLON_OUTPUT_POLICY_V1=y`），所以待机时 `body.presence.set` 的 motion、face、cue 三项都判为不允许，`applied=0`（`eidolon_voice_controller.cc:3125-3137`）。这是代码阅读结论，需要上机确认 presence 反射在输出策略上线后是否还在待机生效。

宠物行为大多发生在待机，必须先定规则。建议：本地反射不含 Host 生成的内容，以 Owner 的常驻输出策略 `policy_.allowed` 为准，而不是会话选择 `selected_`。Owner 允许了 Motion、Expression、AudioCue，设备就可以在待机用它们做本地反射；会话中仍按会话计划。这不改契约，只是给 gate 加一个读取 Owner 常驻许可的方法。**这个语义需要 Owner 拍板。**

#### 4.5.2 隐式通道：intent 手势参数化

- channel 映射表使用 stance、intensity、pace：intensity 决定次数和幅度，pace 决定停留，stance 选变体（playful→bounce、warm→lean_in）。
- 新手势名先进设备白名单和 clip 表，再进 channel 映射，不新开 op。
- 映射表留在 channel，和面部 `_GESTURES` 放在一处，不在 SDK 另建第二个编译器。

#### 4.5.3 显式通道：扩 profile 版本

- 表演类（look_around、bow、tilt、dance）加到 `StackChanHeadAction`，升为 `stackchan.head.v2`。设备 manifest 声明 v2；`ParseSessionOutputPlan` 目前只接受 v1 这一个精确值，需要同时接受 v1 和 v2。
- dance 的灯光是设备 clip 的装饰轨，不作为模型可寻址的动作，与工具描述「没有灯光动作」一致。
- 超过 5 s 的表演需要把运动 deadline 改为 clip 自带上限，由设备侧强制。

#### 4.5.4 触摸上行

- 按现有标准建模：manifest 的 `events` 声明 `sense.touch`；Owner 权限比照 `InputSelection.microphone` 增加触摸输入许可。
- 节流：一次抚摸只报 start 和 stop。
- agent 对「被摸」开口，依赖主动发起回合。这与 co-presence M0 尚缺的「Hub presence→proactive 桥」是同一块，复用它，不另建。
- 屏幕点按仍是开始对话的入口。头顶触摸只做宠物反应、不开对话，与出厂分工一致。

#### 4.5.5 契约改动顺序

SDK golden → `generate.py` 重生 `catalog.json` → esp32 lock。ESP32 侧 `presentation_catalog.h` 是生成物，不能手改。

### 4.6 owner 可配置项（手机端，后做）

`pet_mode`（总开关，默认开）、`idle_motion`（空闲动作，考虑电池 + 出厂 issue #95）、`touch_reactions`、`sounds`、`sleep_after_min`、`quiet_hours`。设备侧读 Hub 配置（现有 `config.refresh`）。

---

## 5. 分阶段落地（2026-10-31 修订）

### P0 · 设备侧，无契约改动

0. 修 §4.1 缺陷 A，上机确认对话结束后待机能动头。
1. 定 §4.5.1 待机本地输出规则（需 Owner 拍板），给 gate 加读取 Owner 常驻许可的方法；上机确认待机 presence 反射恢复。
2. Si12T 驱动移植（出厂 `hal_head_touch.cpp`）+ 手势识别器（tap/double/long/stroke/rapid/swipe/zone），host 单测覆盖识别器。
3. `AnimationClip` 表 + 仲裁器，服务端动作优先抢占、本地让路；迁入现有 7 个手势（含 10 月新增的 `home`）作回归基线。
4. §3.3 触摸反应第一批 clip（tap / stroke / rapid_taps / swipe）。
5. 待机活性：呼吸、gaze saccade、随机看、困倦→睡眠→被摸醒。对话聆听段只动脸和灯。
6. 嘴型同步：取 `on_audio_render_reference` 在渲染侧算的播放 RMS。它排在 10 月新增的播放预缓冲之后，口型不会领先声音。
7. 接上闲置资产：5 个 decorator、`Sleepy`；修 `"sleepy"` 被静默丢弃。
8. 合成音效库：purr / chirp / huff / yawn / squeak，按 §4.5.1 的 AudioCue 规则门控，对话聆听段禁播。
9. RGB 阶段色（听绿、想蓝呼吸、说蓝脉动、闲暗）和触摸色反应。

### P1 · 情绪模型、传感器、可选声学实验

10. §4.4 三标量情绪 + NVS 持久化 + 调制活性和 clip 选择。
11. BMI270：摇晃眩晕、被举起惊吓、倾斜补偿。LTR-553：靠近先抬眼、暗光困倦。
12. 可选：聆听段微动的声学实验（§4.1 第 4 条）。

### P2 · 语义层（在 `stackchan.head.v1` 上扩展）

13. channel 隐式手势参数化（§4.5.2）。
14. `stackchan.head.v2` 表演类动作（§4.5.3）。
15. `sense.touch` 上行 + 复用 proactive 桥（§4.5.4）。

### P3 · 打磨

16. 视觉跟随（M0 sense.attention → yaw/pitch，接 ATK 眼）。
17. 手机端宠物设置；clip 作者工具（导入出厂舞蹈编排）。

---

## 6. 风险与约束

- **待机本地输出权限**（§4.5.1）是唯一需要产品决定的点。不定下来，待机期间所有宠物行为都会被输出门拒绝。
- 舵机电源与 mic 的冲突已收窄到对话聆听段（§4.1），不再是整个方案的前置。
- 本地 clip 若沿用 `BUSY` 互斥，会让服务端动作失败，agent 会对用户说错话（§4.2）。
- 内部 DMA SRAM 极紧（memory 家族），新功能不开新任务：动画播放挂在现有 50 Hz `UpdateLoop`，触摸轮询挂在现有 LVGL 20 ms timer 或同一 tick，情绪模型无任务。
- 空闲动作耗电与舵机寿命：默认幅度小、频率低，`idle_motion` 可关；电量低（INA226/AXP）自动降到只动脸。
- 本地音效在半双工没有 AEC：对话聆听段禁播本地 sfx，否则进 STT。
- 触摸板在头顶，摸的时候可能连带碰到屏幕：FT6336 与 Si12T 事件要互斥（屏幕点按仍是 UI 操作，不触发宠物反应）。
- 契约扩展要走 SDK 生成链，`presentation_catalog.h` 禁止手改。

## 7. 参考链接

- M5Stack StackChan 官方文档（硬件清单、出厂行为）：https://docs.m5stack.com/en/StackChan
- 出厂固件源码：https://github.com/m5stack/StackChan （`firmware/main/hal/hal_head_touch.cpp`）；BSP：https://github.com/m5stack/StackChan-BSP ；触摸传感器 API：https://docs.m5stack.com/en/arduino/stackchan/touchsensor
- 空闲动作耗电 issue：https://github.com/m5stack/StackChan/issues/95
- stack-chan 头顶触摸移植 issue（Si12T 读法、手势阈值）：https://github.com/stack-chan/stack-chan/issues/410
- stack-chan 固件（liveness / mod 架构）：https://github.com/stack-chan/stack-chan
- m5stack-avatar（Emotion / LipSync）：https://github.com/stack-chan/m5stack-avatar
- Vector 角色设计指南（多轨动画、emotion engine）：https://randym32.github.io/Anki.Vector.Documentation/guides/Vector%20character%20design%20guide.html
- Eilik 触摸反应设计：https://www.yankodesign.com/2021/12/23/meet-eilik-a-feisty-little-ai-robot-that-lives-on-your-desk-like-a-tiny-tamagotchi-with-a-personality/
- Focus Bot（ESP32 小内存情绪模型样板）：https://github.com/Rayan7717/Focus_Bot_Desktop_Pet_Based_on_esp32C3
- ESP32 触摸宠物机器人参考：https://github.com/tuptus922-tech/esp32-robot 、https://github.com/SukunDev/ESP32-Pet-Robot

---

## 8. 多样性的构成（表现词汇总表）

### 8.1 脸：四层叠加
- 基态情绪 6：平静 / 开心 / 生气 / 难过 / 疑惑 / 困倦（复用 avatar `Emotion`），分钟级变化，由情绪模型决定。
- 瞬时表情 8：现有 `face-core-1` catalog，叠在基态上，百毫秒到秒级。
- 装饰物 5：heart / shy / dizzy / sweat / angry（已移植未接）。
- 微行为：眨眼间隔随机且困时变慢；gaze 每 2–5 s saccade；说话嘴随播放 RMS 开合；听时眼大并朝声源微动；想时眼珠上翻；睡着慢垂眼皮、惊醒先睁一只眼。

### 8.2 头：三层叠加
- 连续层：呼吸（pitch ±1.5°，0.15–0.25 Hz）；空闲随机看（4–12 s，幅度方向随机）；困倦渐低头。
- 离散手势 ≈20：nod / shake / tilt / perk_up / droop / glance / stretch / shiver / recoil / nuzzle / bow / peek / look_around / bounce / dizzy_circle / doze_nod / turn_away / follow / wake_wobble / sneeze。
- 参数：amplitude / times / speed / hold，外加 15–20% 随机抖动；由 arousal 调制幅度与速度。

### 8.3 声（全合成，无资产）
purr（AM 低频，随抚摸渐强渐慢）/ chirp_up（疑问）/ chirp_down（失望）/ double_chirp / huff（生气）/ yawn / squeak（惊）/ glide（跟随滑动）/ beat（跳舞）。音高、时长随 valence 偏移。

### 8.4 灯（12 颗两排）
听绿常亮 / 想蓝慢呼吸 / 说蓝随音量脉动 / 闲极暗呼吸 / 抚摸暖粉呼吸 / 生气红快闪 / 触摸侧先亮 / 舞蹈跑马跟节拍。

### 8.5 组合乘法
| 因子 | 取值 |
|---|---|
| 触发情境 | 触摸 8 + IMU 4 + 对话 intent 9 + 空闲事件 6 |
| 情绪基态 | 6（决定变体与幅度） |
| clip 变体 | 每个 2–3 |
| 参数随机 | 幅度 / 时长 / 时机各 15–20% |

约 40 个手写 clip → 数百种可辨识反应。示例：
- 摸头：平静→闭眼蹭+呼噜+暖粉；开心→先抬头看你再蹭、呼噜更响；刚被拍→转开+汗珠+无声；睡着→哈欠、慢睁眼、再蹭。
- 回桌：精力足→perk_up+开心+白闪+啾；困→doze_nod 惊醒+眯眼+小点头。
- agent celebrate：playful→bounce×2+shake+delight+蓝闪；warm→慢 nod+soften+柔灯。

### 8.6 时间线
开机精神 → 交互后兴奋 → 冷落 30 min 困倦 → 60 min 睡着（偶发小动）→ 被吵醒不情愿。靠 energy 标量 + Hub 时间，不加动画。

---

## 9. 复核记录

### 2026-10-31（对照 9-21 之后的约 130 个提交和兄弟仓库）

| 变化 | 来源 | 对方案的影响 |
|---|---|---|
| 服务端落地 `stackchan.head.v1`：channel 隐式 intent 手势 + agent 显式 `stackchan_head` 工具 | SDK b210617、agent 2457712、channel 4437e17 / 0b9b4d4、本仓 0d13718c | §1.5 和 §4.5 原设想作废，改为在其上扩展 |
| 设备手势白名单新增 `home` | 0d13718c | clip 回归基线为 7 个手势 |
| StackChan 关闭唤醒词、屏幕点按开对话；对话结束无条件关采集 | 构建脚本、`SuspendConversationAudio` | 待机无声学冲突，§4.1 声学实验降为 P1 可选 |
| 对话结束不恢复舵机电轨（缺陷 A） | 代码阅读，未上机 | 新增 P0 第 0 项 |
| 输出门为会话作用域，待机拒绝一切输出 | `output_policy.cc`，未上机 | 新增 §4.5.1，P0 第 1 项需 Owner 决定 |
| 麦克风成为独立 Owner 输入许可 | c7ba1fc3、SDK `InputSelection` | 触摸上行比照建模 |
| 控制器事件队列出过溢出 | `docs/validation/controller-inbox-20260923.md` | §4.2 规定触摸采样不进控制器队列 |
| 播放预缓冲、渲染侧 RMS | fa381617、`livekit_board.cc` | 嘴型同步取渲染侧 RMS，口型不领先声音 |
