# StackChan 桌面宠物化：动作 / 表情 / 触摸反馈整体方案

状态：方案草案（2026-09-21），未开始实现。

## 0. 一句话结论

现在的 StackChan 是「一台会转头的对话终端」，不是宠物：头部只有 6 个离散手势、脸只有 8 个手势 + 4 个基态、没有任何空闲自主行为、没有嘴型同步、机身上的三区触摸板 / IMU / 接近光传感器 / 12 颗 RGB 全部闲置或只在「唤醒」时亮一下。要变成宠物，缺的不是「再加几个手势」，而是三层东西：**一个常驻的活性层（idle liveness）、一个本地反射层（touch / IMU 反射）、一套多轨动画数据格式（头 + 脸 + 灯 + 声 同步）**，再加一条把这些接进 agent 语义层的契约通路。

同时有一个硬约束必须先解决：**半双工模式下 mic 一开舵机电源就被切断**，而 mic 在 agent 不说话时一直是开的。不解这个，任何空闲动作、任何「摸头点头」都动不了。

---

## 1. 现状盘点（含代码位置）

### 1.1 舵机 / 身体

- 硬件：Feetech SCS0009 串行舵机 ×2（yaw id1 / pitch id2），UART1 1Mbaud；软件限位 yaw ±128°、pitch 3°…87°（`main/boards/m5stack-stackchan/config.h:93-96`）。弹簧阻尼插值，50 Hz tick（`servo.cc:133-150`、`stackchan_body.cc:117-150`）。
- 手势表只有 6 个，全部硬编码在 `RunGesture()`（`stackchan_body.cc:335-372`）：`nod / shake / perk_up / droop / glance / wake_wobble`。每个都是「几个固定角度 + 固定 hold」，无缓动曲线、无随机、无幅度参数。
- **没有任何空闲行为**：无呼吸、无随机看、无眨眼配合的转头。唯一的「空闲」是舵机自动卸力（`servo.cc:65-72`）。
- 舵机电源门控：`SetCaptureQuiet(true)` 直接切断 VM EN 并冻结 motion（`stackchan_body.cc:152-175`）；由 `eidolon_voice_controller.cc:3586` 推导：半双工 = `mic_enabled_ && !playback_active`。**结论：只要在会话里且 agent 没在说话，头就是断电的。**
- 安全护栏已完备：`safety.stop`、5 s 运动 deadline、TTL、pitch 堵转保护、`BUSY` 互斥。这一层不需要动，新方案全部在它下面跑。

### 1.2 表情 / 脸

- 渲染：LVGL + 移植的 M5 avatar 皮肤（`main/eidolon/avatar/skins/default/`），`CompanionFaceView` 驱动。
- 表情词汇 = SDK 契约 `face-core-1` 的 8 个 gesture（`attend/affirm/ponder/question/soften/delight/hesitate/attention`，`expression/generated/presentation_catalog.h:16`）+ 4 个基态（`Idle/Listening/Thinking/Error`，`expression/core/runtime.h:10`）。每个 gesture 是一个静态 `FacePose`（`runtime.cc:15-28`），在 plan 的 step 上做包络。
- 眨眼有（4.3 s 周期），**嘴不动**：`mouth_open` 只在 `Delight/Attention` 两个 pose 里是非零，`AgentSpeaking` 阶段没有任何东西驱动嘴（`companion_face_view.cc:461`）。
- 已经移植但**一行都没被调用**的资产：`Emotion {Neutral, Happy, Angry, Sad, Doubt, Sleepy}`（`avatar/avatar/elements/emotion.h`）和 5 个 decorator（`angry / heart / shy / dizzy / sweat`，`avatar/decorators/`）。
- 本地反射 `AvatarExpress()` 只认 3 个字串（happy/thinking/sad），且 `body.presence.set` 的 legacy 分支传 `"sleepy"` 会被静默丢掉（`companion_face_view.cc:428-431`）。

### 1.3 触摸 / 传感器（机身）

- 出厂硬件（M5 官方文档）：**Si12T 三区电容触摸板（I2C 0x68，头顶）**、BMI270 六轴 + BMM150 磁力计、LTR-553 接近/光感、IR 收发（G5/G10）、ST25R3916 NFC（0x50）、INA226 电量（0x41）。
- 本仓库：**以上全部未接**。grep 无 `0x68 / bmi270 / ltr553 / si12t`。唯一的「触摸」是屏幕 FT6336，Hub 模式下只作 LVGL 指针（`m5stack_stackchan.cc:236-260`）。
- 出厂固件 `firmware/main/hal/hal_head_touch.cpp` 的读法已查清（见 §3.3），可直接移植。

### 1.4 声音 / 灯

- 本地音效只有 3 个合成音（identify 双音、startup chime、roll call），全部在 `eidolon_local_feedback.cc`，无 wav 资产。
- RGB 只有一个效果 `RgbMarquee`（青色跑马灯），只在 `body.presence.set awake` 时触发一次；agent 说话 / 听 / 想的时候灯全黑。出厂固件是「绿=听、蓝=说、灭=闲」。

### 1.5 语义层（服务端）

- agent 每轮通过 `eidolon_respond` 工具输出 `PresentationCandidate{intent, stance, intensity, pace}`（`eidolon_agent/domain/agent/presentation.py`），intent 9 种（`acknowledge/confirm/consider/clarify/comfort/celebrate/decline/notify/none`）。这个候选被编译成 `ExpressionPlan`（面部）下发。
- **头部动作完全不在这条通路里**。`head.gesture` 目前只有两个调用方：`body.presence.set`（awake→wake_wobble / warm→droop）和 admin 手动。agent 说「太好了！」的时候脸会 delight，头一动不动。

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
| 回应 intent | celebrate→蹦跳(pitch bounce)+摇头；comfort→缓慢低头靠近；decline→摇头；clarify→歪头；acknowledge→轻点头 | 现有 face plan | 与 stance 对应的色相 |

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

供 agent 工具调用或 owner 直接说的命名动作：`dance`（yaw 大幅摆动 + pitch 弹跳 + 灯随节拍，可配本地合成节拍音）、`look_around`（左中右扫视）、`bow`（鞠躬）、`nuzzle`、`stretch`、`shiver`、`peek`（探头）、`sneeze`、`yes/no`（现有 nod/shake 加幅度和次数参数）、`follow_me`（打开视觉跟随：ATK/摄像头 bbox → yaw/pitch，M0 sense.attention 的下游）。

---

## 4. 架构方案

### 4.1 先解决舵机断电（P0 前置，最高优先）

现状 `SetCaptureQuiet(capture_on)` 把「mic 开」和「舵机电源关」硬绑在一起（`eidolon_voice_controller.cc:3604`），根因是舵机电源轨的开关啸叫（~3 kHz）会把 ADC 打满（见 memory：StackChan uplink audio）。选项：

1. **按阶段而不是按 mic 门控**（推荐先试）：只在 `UserSpeaking / Recording`（STT 真正在吃音频）时断电；`Silent` 阶段 mic 虽开但只做 VAD/EOT，测一下啸叫是否触发服务端 VAD 误判。若不触发，空闲活性和触摸反应就都能动。
2. **卸力而不断电**：验证「卸力 + 电轨常开」的啸叫是否真的比断电差（memory 说只有切电轨才能静音，但当时是全双工 AEC 场景；半双工 + 服务端 VAD 的容忍度不同）。
3. **动作窗口调度**：在 `Silent` 阶段以「短暂通电 → 动 → 断电」为单位做动作（200 ms 上电稳定 + 动作 + 断电），呼噜/呼吸类连续动作不可行，但随机看、触摸反应可行。
4. 硬件层：给 VM 轨加 LC 滤波 / 换开关频率（问 M5，非本仓库能解）。

必须先量化再选：录 STT WAV 对比（memory 里有验证方法：期望有调制、无 32768 削顶、ZCR 0.05–0.15）。

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

### 4.5 与 agent 语义层的契约（按「先找标准」原则）

已有标准：agent 输出 `PresentationCandidate{intent, stance, intensity, pace}` → 编译成按 profile 的 plan（现在只有 `eidolon.face.v1`）。**正确扩展点是给编译器加一个身体 profile，而不是让 LLM 直接点舵机角度。**

1. SDK 新增 `eidolon.body.v1` 目录（catalog `body-core-1`）：`nod / shake / tilt / perk_up / droop / bounce / lean_in / recoil / look_around / bow` 等，同样有 intensity / variant，同样受 `Motion` 输出策略门控（现有 `MOTION_NOT_SELECTED` 路径复用）。
2. 编译规则 = intent × stance → face plan + body plan（例：celebrate/playful → face delight + body bounce；comfort/warm → soften + lean_in；decline → question + shake）。这样 agent 一行代码不改就让全部 9 种 intent 有头部动作。
3. 命名表演（dance/look_around/peek）作为 agent 工具 `perform(clip, params)`，通过新 op `animation.play`（或复用 `head.gesture` 扩名单）下发，仍走 session 授权 + 5 s deadline（表演类需要放宽 deadline，改成 clip 自带 `max_duration_ms ≤ 30 s`，由设备侧强制）。
4. 触摸事件上行：`sense.touch{gesture, zone, duration}` 发给 Hub → agent 可以对「被摸」说话（「嘿嘿，别挠我～」）。这是宠物感从「反射」升到「有意识」的一步，但要有节流（一次抚摸只上报一次 start/stop）。
5. 契约改动顺序按 memory：SDK golden → `generate.py` 重生 `catalog.json` → esp32 lock；ESP32 侧 `presentation_catalog.h` 是生成物，不能手改。

### 4.6 owner 可配置项（手机端，后做）

`pet_mode`（总开关，默认开）、`idle_motion`（空闲动作，考虑电池 + 出厂 issue #95）、`touch_reactions`、`sounds`、`sleep_after_min`、`quiet_hours`。设备侧读 Hub 配置（现有 `config.refresh`）。

---

## 5. 分阶段落地

### P0 · 设备侧，无契约改动，一周量级，UX 提升最大

1. §4.1 舵机电源门控实验 + 结论（决定 P0 其余项的上限）。
2. Si12T 驱动移植（出厂 `hal_head_touch.cpp` 逻辑）+ 手势识别（tap/double/long/stroke/rapid/swipe/zone），host 单测覆盖识别器（`tests/` 已有 host 测试骨架）。
3. `AnimationClip` 表 + 仲裁器；把现有 6 个手势迁入；新增 §3.3 反应表的第一批 clip（tap / stroke / rapid_taps / swipe）。
4. Liveness 层：呼吸、gaze saccade、随机看、困倦→睡眠→被摸醒。
5. 嘴型同步：`AgentSpeaking` 时用播放 PCM RMS 驱动 `mouth_open`（播放侧 RMS 已有 `eidolon_livekit_board_recent_playback_rms_ppm`）。
6. 接上闲置资产：5 个 decorator、`Sleepy` 情绪；修掉 `"sleepy"` 被静默丢弃的 bug。
7. 合成音效库：purr / chirp / huff / yawn / squeak（延续 `eidolon_local_feedback` 的合成路线，零资产）。
8. RGB 阶段色（听绿 / 想蓝呼吸 / 说蓝脉动 / 闲暗），触摸色反应。

### P1 · 情绪模型 + 更多传感器

9. §4.4 三标量情绪 + NVS 持久化 + 调制 liveness/clip 选择。
10. BMI270：摇晃眩晕、被举起惊吓、倾斜补偿。LTR-553：靠近先抬眼、暗光困倦。

### P2 · 语义层契约

11. SDK `eidolon.body.v1` + intent 编译规则 → 9 种 intent 全部带身体语言。
12. `animation.play` + agent `perform` 工具 + 表演类 clip（dance/look_around/bow/peek）。
13. `sense.touch` 上行 + agent 对触摸的口头反应。

### P3 · 打磨

14. 视觉跟随（M0 sense.attention → yaw/pitch，接 ATK 眼）。
15. 手机端宠物设置；clip 作者工具（导入出厂舞蹈编排）。

---

## 6. 风险与约束

- **舵机电源 vs mic**（§4.1）是唯一可能让整个方案打折的点，必须第一个做实验。
- 内部 DMA SRAM 极紧（memory 家族），新功能不开新任务：动画播放挂在现有 50 Hz `UpdateLoop`，触摸轮询挂在现有 LVGL 20 ms timer 或同一 tick，情绪模型无任务。
- 空闲动作耗电与舵机寿命：默认幅度小、频率低，`idle_motion` 可关；电量低（INA226/AXP）自动降到只动脸。
- 本地音效在半双工没有 AEC：`UserSpeaking` 期间静音本地 sfx，否则进 STT。
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
