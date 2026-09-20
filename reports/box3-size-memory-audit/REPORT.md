# BOX-3 固件体积、分区与运行时内存审计

审计日期：2026-09-20（Asia/Shanghai）。范围：只读审计原工作区、当前隔离目录中的可逆编译/重链接实验。未刷机，未改生产分区，未修改原工作区，未提交或合并。

**建议先收窄默认音频编解码注册，不先扩分区。** 实验把默认注册限定为 Opus/PCM 后，image 从 **4,223,504 B 降到 3,489,808 B**，减少 **733,696 B（716.5 KiB，17.37%）**；每个 OTA 槽空闲从 **36,336 B（0.853%）**增到 **770,032 B（18.08%）**；静态内部 RAM 少 **6,964 B**。已完成编译、链接、image 生成和尺寸验证，**未完成实机语音、重连、OTA 回归**。这是优先候选的实测收益，不是可直接发布的固件。

## 1. 基线与可复核性

原工作区：`/Users/manson/ai/eidolon/eidolon-client-esp32`。

实际产物目录：`/Users/manson/ai/eidolon/eidolon-client-esp32/build/eidolon/esp-box-3`。

本报告目录：`/Users/manson/.codex/worktrees/0f1f/eidolon-client-esp32/reports/box3-size-memory-audit`。下文证据相对路径均相对此目录；完整原始输入保存在 `evidence`，实验放在 `experiments`。

| 项目 | 核实结果 |
|---|---|
| 原工作区 HEAD | `f0e6d0469335724cc239ff47895e34319bdf5114` |
| 实際 IDF | `/Users/manson/.espressif/v5.5.4/esp-idf`；Python `idf5.5_py3.13_env` |
| 编译器 | Xtensa ESP GCC 14.2.0，工具目录 `esp-14.2.0_20260121` |
| image | `0x407210` = 4,223,504 B |
| image SHA256 | `1843c1e3a7c8ff8224d83f7eb8fdaf20a95e7aab81c41547fe4ec1eb36964694` |
| ELF SHA256 | `b81aa939e33a7943153beecafc0e42e9a2bcfd5a5d000396aca8a12e577f4939` |
| map SHA256 | `35b4dc3beea12b590e070a50bb0740b960ab84fedaa963a5f8940d8ab7314768` |
| 编译数据库 | 2,788 条记录；板级源文件 22 个，其中非 common 的板型仅 `esp-box-3/esp_box3_board.cc` |
| 配置 | BOX-3、ZH_CN、Hub、Companion face、设备端 AEC low-cost；`-Os`，链接 `--gc-sections`、`-fno-lto` |
| 关键存储/内存 | 16 MB Flash、双 OTA 回滚开启；PSRAM 80 MHz、malloc 可用、internal threshold 2,048 B、内部保留 98,304 B；TLS external alloc |

`evidence/source-status.txt` 和 `source.patch` 保存了原工作区未提交差异：Application UI 队列/代次、LVGL 更新、UI labels/presenter/mapper、Hub activator、companion view/layout 及相关测试。未跟踪的显示说明和 presenter 测试保存在 `evidence/source-untracked`。关键源文件快照在 `evidence/source`。**没有把当前 worktree 的默认分支当作刚构建的基线。**

实验复用实际构建中的对象、静态库和链接脚本；先不改变任何对象重链接，输出 ELF 与原 ELF 完全一致，使用与 build.ninja 一致的 image 参数后 `.bin` 的 SHA256 也完全一致。随后仅替换 codec 注册库中的两个对象。这比从默认分支重新配置更直接地保留了未提交的显示修复。源文件/依赖不全量重编译，故该实验不替代后续正式 clean build。

主要证据：`manifest.json`（大小、时间戳、SHA256）、`build.log`（原 `/tmp/box3-firmware-build.log`）、`original.map`、`sdkconfig.esp-box-3`、overlay、`dependencies.lock`、`idf_component.yml`、`link-command.txt`。

**结束复核发现并发构建变化：** 原工作区bin和ELF随后被其它工作更新，大小不变，map/config和git diff均未变；新的bin SHA256为 `738106426cb576512ae293fdba5ce0ae6a5814a565c5ab9f20c13330c0dccb3a`，ELF为 `cd3e3f9c73e6f44cb29d5587cb6b365cee4e5b2f05eeea606dc7ea2e05bfdd61`。逐section比较只发现 `.flash.rodata` 两个字节不同，未将其原因作无证据猜测。本报告的实验仍明确针对开始时的已归档基线；容量结论不变，不声称候选是从这份较新ELF产生。见 `end-of-audit-drift.json`、`end-of-audit-section-diff.json`、`source-end.patch`。重新运行脚本会因哈希保护拒绝混用新原件，必须建立新快照后再做新的实验。

## 2. Flash：准确口径与逐层归因

### 2.1 ELF section 和 image

| section | 字节 | 解释 |
|---|---:|---|
| `.flash.text` | 2,855,320 | Flash 中执行的指令 |
| `.flash.rodata` | 1,233,584 | 常量、字体、提示音、证书、字符串等 |
| `.flash.appdesc` | 256 | 真正应用描述 |
| `.iram0.text` | 99,903 | 启动时装入 IRAM；也占 image |
| `.iram0.vectors` | 1,028 | 向量；也占 image |
| `.dram0.data` | 33,260 | 初始化数据；占 image 和内部 RAM |
| `.dram0.bss` | 34,856 | 内部 RAM，零初始化，不按内容占 image |
| `.rtc.force_slow` | 32 | RTC 初始化数据 |
| `.rtc_reserved` | 40 | RTC NOBITS 保留区域 |
| 实际 `.bin` | **4,223,504** | 分段、对齐、image 头/校验后的最终占用 |

证据：`size.txt`、`elf-sections.json`。size 工具的 Total image size 为 4,223,351 B，和 bin 差 153 B；以 bin 作为分区余量依据。`.dram0.dummy`、`.flash_rodata_dummy`、`.ext_ram.dummy` 是映射/地址布局用的 NOBITS，不应当作真实额外堆或资源；`.flash.rodata_noload` 12,646 B 也不是 image payload。不要将 55.6 MB 的 ELF（含调试信息）当成烧录大小。

### 2.2 主要 archive

以下为 `esp_idf_size --archives` 的归因值（B），Flash列是该工具的 `flash_total`，包含加载到 RAM 的初始化内容；不等于单纯 `.flash.text + .flash.rodata`。合并字符串、对齐与 relaxation 会影响归属，排序适合定位候选，不能把整库数字当作可删除收益。

| archive | Flash .text | Flash .rodata | DRAM data / bss | IRAM text | flash_total |
|---|---:|---:|---:|---:|---:|
| esp_audio_codec | 609,420 | 278,257 | 1,098 / 8 | 5,836 | 894,611 |
| main | 329,677 | 163,122 | 175 / 2,430 | 0 | 492,974 |
| lvgl | 225,766 | 25,858 | 0 / 480 | 0 | 251,624 |
| esp_app_format | 479 | 220,546 | 0 / 10 | 0 | 221,281 |
| stdc++ | 136,427 | 78,389 | 156 / 4,325 | 0 | 214,972 |
| xiaozhi-fonts | 410 | 210,961 | 0 / 0 | 0 | 211,371 |
| dl_lib | 174,429 | 7,289 | 5,936 / 8 | 1,789 | 189,443 |
| net80211 | 124,549 | 13,979 | 936 / 7,603 | 0 | 139,464 |
| libc | 99,527 | 6,889 | 364 / 764 | 0 | 106,780 |
| mbedtls | 30,287 | 71,333 | 0 / 244 | 0 | 101,620 |
| lwip | 84,216 | 3,581 | 12 / 4,054 | 26 | 87,835 |
| esp_audio_processor | 74,787 | 3,490 | 3,084 / 52 | 1,768 | 83,129 |
| mbedcrypto | 69,662 | 7,012 | 92 / 280 | 80 | 76,846 |
| freertos | 1,379 | 41,885 | 3,116 / 1,081 | 18,133 | 64,513 |
| esp_image_effects | 63,055 | 361 | 0 / 0 | 0 | 63,416 |
| peer_default | 45,779 | 2,112 | 0 / 438 | 0 | 47,891 |
| esp-ml307 | 15,399 | 14,816 | 0 / 0 | 0 | 30,215 |

完整清单见 `archives.txt` 和 `archives.json`。

**字符串池误归因：** map 第 218391–218393 行，`esp_app_desc.c.obj` 的 `.str1.1` 由原先 `0xdf` B relaxed 为 `0x35d7a` B（220,538 B），是链接器合并字符串的结果。真正 `.flash.appdesc` 只有 256 B。它不是一份 216 KiB 的独立应用元数据，不能通过删除 app description 回收。其它对象中重复/合并字符串也不能简单累加出精确裁剪收益。

### 2.3 主要对象、符号和引入路径

| 对象/符号 | 当前占用或符号大小 | 归因/取舍 |
|---|---:|---|
| `font_noto_basic_20_4.c.obj` | rodata 122,896 B | 启动/回退文字字体；最大 glyph bitmap 102,156 B |
| `font_awesome_30_4.c.obj` | rodata 58,616 B | SpiLcdDisplay 默认主题强制创建 large icon font |
| `font_awesome_20_4.c.obj` | rodata 28,365 B | 当前网络/电池/麦克风等图标需要保留其覆盖集 |
| `lv_font_montserrat_14.c.obj` | rodata 14,913 B | LVGL 默认字体，不能仅删 Kconfig 而不替换默认字体 |
| `x509_crt_bundle.S.obj` | rodata 68,987 B | 完整 CA bundle；不是可无条件删除的资源 |
| `dl_tie728_sr_s8_conv2d.S` | text 102,930 B | ESP-SR/DL 内核，不能按“有人脸 Kconfig”误判成人脸模型 |
| `sigmoid_table_5_8` | data 5,680 B | 内部初始化数据，迁移 const/外部需验证闭源库与实时访问 |
| `imgfx_color_convert_lib` | text 44,973 B | image conversion 注册/通用处理链，后续核对音频-only路径是否可拆 |
| `lc3_constant_fx.c.obj` | rodata 44,529 + data 308 B | 全量 codec 注册引入，实验可随 LC3 路径回收 |
| `aac_rom.c.obj` | rodata 22,968 B | 全量 codec 注册引入 |
| `esp_mp3_dec.c.obj` | text 12,469 + rodata 7,913 B | 全量 codec 注册引入 |
| `lodepng.c.obj` | text 16,279 + rodata 8,852 B | PNG/截图等通用显示功能，需要功能核对 |
| `wlocale-inst.o` / `locale-inst.o` | flash_total 36,975 / 36,964 B | C++ locale/iostream 引入路径候选，不是可直接删掉的两个文件 |
| `HubOnboardingClient::ContinueCanonicalClaim` | text 10,202 B | Owner 认领关键路径，保留 |
| `opus_encode_native` / `celt_encode_with_ec` | text 8,438 / 9,049 B | Opus 主路径，保留 |
| `lv_draw_sw_blend_image_to_argb8888` | text 4,864 B | LVGL 多像素格式支持候选，收益需实际重链接 |

证据：`objects.txt`、`symbols.txt`、`linked-symbol-names.txt`，map 开头的 archive inclusion 原因与末尾 cross-reference。`nm` 中 `D/d` 并不能单独证明是内部 DRAM；`0x3c...` 符号可能位于 Flash rodata，需看 section/地址。

### 2.4 “源码存在 / 编译 / 链接 / assets”区分

| 内容 | 观察 | 结论 |
|---|---|---|
| 其它板型 | 编译库包含 21 个 common 源文件，专属板型仅 BOX-3 | 没有证据表明把所有实体板型实现打进 image |
| ML307/EC801 蜂窝 | map inclusion 可见 AtModem 引用链，但最终 symbol table 无 AtModem/Ml307/Ec801 类实现；common ml307 对象仍归到少量 rodata | 不可把 map 开头“取出 archive member”当最终存活；也不能整删 esp-ml307 |
| esp-ml307 的通用网络层 | EspNetwork/HTTP/TLS/TCP/UDP/WebSocket 等存活，EspMqtt 对象亦有约 2 KiB归因 | Wi-Fi 板依赖此通用工厂；先拆能力/工厂，后量化 MQTT 等未用分支 |
| 旧 MqttProtocol/WebsocketProtocol | 最终 symbol table 无这些协议类实现；size 仍归到约 1,389 / 1,013 B对象数据 | 主实现已 GC，残留池化常量不等于整个协议栈；不要宣传巨额可省 |
| 人脸模型、摄像头/视频配置 | 有 Kconfig、生成的 `.espdl.S` 文件和被构建依赖；最终无 human_face 符号，assets index 也无它们 | “模型在磁盘上”不是“烧进此 app”；清配置可减构建复杂度，但不能将生成汇编文件大小计作 image 收益 |
| wake word | `WAKE_WORD_DISABLED=y`；SR 的 wake-word 默认选项还存在；assets 无 `srmodels.bin` | 不应声称当前 assets 包中有几 MB 唤醒模型；AFE/DL仍是实际语音依赖 |
| 多语言/字体 | 选择 ZH_CN；实际链接的是有限内置字体，assets另有大中文字库 | 没有全语言包链接证据；内置回退字体与外部中文字库不是天然重复可删 |
| 配网页面 | 两个 `wifi_configuration*.html.S.obj` 最终 size 为 0 | 392 KiB `.S` 文本不是可回收 392 KiB Flash；配网能力本身仍需保留 |
| 提示音 | 存活 OGG 对象合计 40,273 B；welcome/success/upgrade/wificonfig对象为0 | 认领数字、错误、低电量等有功能意义；不能整删以勉强挤出当前余量 |
| 调试/示例 | `-Os`已开，INFO日志、assert开启；LVGL大量通用像素格式和widget配置开启 | 配置开启不保证代码存活；按最终符号/行为裁剪。关闭assert/信任校验不列优先项 |
| 重复依赖 | 链接行重复列出 archive（静态库依赖组常见），有音频/简单解码/渲染多层 | 重复列名不等于重复复制机器码。未发现可用“删重复库”直接保证的大额收益 |

## 3. 分区、OTA、assets 与迁移

二进制 partition table 经仓库 `partition_contract.validate_build()` 验证：与 CSV 一致，分区和写入文件均不越界，16 MB 边界恰好用满。

| 分区 | 起点 | 大小 | 结束（不含） |
|---|---|---:|---|
| nvs | 0x9000 | 0x4000（16 KiB） | 0xd000 |
| otadata | 0xd000 | 0x2000（8 KiB） | 0xf000 |
| phy_init | 0xf000 | 0x1000（4 KiB） | 0x10000 |
| owner_trust | 0x10000 | 0x10000（64 KiB） | 0x20000 |
| ota_0 | 0x20000 | 0x410000（4.0625 MiB） | 0x430000 |
| ota_1 | 0x430000 | 0x410000 | 0x840000 |
| assets | 0x840000 | 0x7c0000（7.75 MiB） | 0x1000000 |

`BOOTLOADER_APP_ROLLBACK_ENABLE=y`，OTA代码调用 `esp_ota_mark_app_valid_cancel_rollback()`。回滚不等于 eFuse 防降级：`BOOTLOADER_APP_ANTI_ROLLBACK` 未启用；也不能据 SOC_SUPPORTED 推断设备已启用 secure boot。此审计未读取实机 eFuse。保留现有证书验证、Owner trust分区和双槽契约；分区脚本要求两个槽等容量，明确不做自动搬迁。

实际烧入 assets 的是 `generated_assets.bin` **7,705,022 B**，余量 **421,442 B（411.6 KiB，5.19%）**。CSV subtype 虽是 spiffs，此产物是 `build_default_assets.py` 打包的 mmap 资源格式，不应按普通 SPIFFS 文件系统开销推断。

| assets 内容 | 字节 |
|---|---:|
| `font_noto_qwen_20_4.bin` | 2,998,916 |
| 21 个 GIF 合计 | 4,703,148 |
| index/table/prefix等 | 2,958 |
| 合计 | 7,705,022 |

目录中的 `expression_assets.bin` 是另一个 **2,448,470 B** 产物，但不在当前 flash_args 的写入列表，不可把它再加进当前烧录体积。也不能仅因它更小就替换 assets；两套包的格式、font、动画行为不同。`Assets::Apply(false)` 仍加载资源，随后 Companion 视图使用主题字体；大中文字库对字幕覆盖有价值。21个传统GIF在当前face是否全部有可达消费者，应再做运行时引用统计；不能靠文件名判定已废弃。

| 方案 | 实际收益/估算 | 迁移与兼容性 |
|---|---|---|
| 收窄 codec 注册 | 实测 app -733,696 B，两槽位置不动 | 无分区迁移；仍须语音回归 |
| 裁剪 assets 中未用 GIF | 上界是 GIF合计4,703,148 B；未证明可全部删除 | 只减 assets，不自动扩大 app；须保留表情能力、字体覆盖与旧固件回滚兼容 |
| 内置字体/提示音搬进 assets | app上界按实际链接资源计；新增运行时元数据/加载开销 | 字体启动回退、资源损坏/缺失、旧 app 与新 assets 格式必须兼容；非优先 |
| 双槽各加0x10000 | 每槽仅多64 KiB，总需挤assets128 KiB；现有包数值上能放下 | ota_1和assets起点变化，需要明确迁移方案；当前余量问题可由codec解决，价值低 |
| 双槽各加0x40000 | 每槽多256 KiB，总占用增加512 KiB，现assets还差102,846 B才放得下 | 还需资源裁剪；不能通过常规 app OTA 顺便安全重排 |
| 单槽/删trust/NVS | 不建议 | 损坏用户要求的回滚与身份存储，不纳入优化 |

共享 assets 没有 A/B 冗余：新旧 app 必须都能使用同一资源版本。资源 OTA 与 app 回滚的组合，是搬移资源时的重要成本；不应仅用“Flash总量有空闲”作决定。

## 4. 内部 RAM、PSRAM 和动态峰值

### 4.1 静态基线与能力约束

静态内部 D/IRAM 合计 **169,047 B**，size 工具显示相应链接区域剩余172,713 B。该值不是运行时 `heap_caps_get_free_size()`，更不是可用最大连续块。FreeRTOS、Wi-Fi、codec、AFE、TLS、队列和图形对象在启动/入会时还会动态分配。

IRAM热点：FreeRTOS text 18,133 B，HAL 11,407 B，esp_hw_support 10,495 B，codec 5,836 B，PHY 5,073 B。data/bss热点包括 Wi-Fi net80211（8,539 B）、HAL（7,144 B）、DL（5,944 B）、stdc++（4,481 B）、lwIP（4,066 B）。这些块部分可以研究迁移，但 ISR、cache-disabled 路径、驱动状态和库 ABI 是约束，不能统一搬 PSRAM。

当前未启用 `SPIRAM_FETCH_INSTRUCTIONS`、`SPIRAM_RODATA` 或外部 BSS。普通 malloc 大于2048 B偏向PSRAM不等于一定成功，也不改变默认 FreeRTOS 队列/栈的内部内存要求。`SPIRAM_MALLOC_RESERVE_INTERNAL=98304` 是能力保留策略，不是可删除后白得96 KiB物理SRAM。

BOX-3历史串口确认16 MB PSRAM；不能只凭16 MB Flash推出PSRAM也是16 MB。当前 image 尚无对应实机启动证据。

### 4.2 当前代码的主要分配账本

| 项目 | 字节/配置 | 所在内存与约束 |
|---|---:|---|
| LiveKit engine event queue | 32 × 296 = 9,472 B payload | 默认 FreeRTOS 内部块，另有控制/allocator开销 |
| LiveKit engine task | 8,192 B | 内部连续栈；辅助对象模型下界2,048 B |
| room_create admission | total 19,712 B，largest ≥9,472 B | 只是创建engine的下界；不保证整个入会/TLS峰值成功，也不充分证明两个顺序分配都能满足 |
| controller worker | 16,384 B | 内部栈；需要跨所有事件路径测高水位 |
| activation worker | 16,384 B | 内部、临时生命周期；TLS/Owner异常路径约束 |
| Owner trust worker | 12,288 B | 配置栈，不能以未创建时内存判断最坏峰值 |
| mic read task（AEC路径） | 8,192 B | 当前 xTaskCreatePinnedToCore 内部栈 |
| AFE communication task | 4,096 B | xTaskCreate 内部栈 |
| PCM ring | 16,000 B + RTOS控制开销 | **BOX-3明确用内部SRAM**，有PSRAM噪声/STT失败记录 |
| renderer raw FIFO | 8×4096 = 32,768 B | media层动态分配；跟踪实际caps/同时存活情况 |
| renderer output FIFO | 100×1024 = 102,400 B | 候选降低PSRAM working set；减小会缩短网络抖动缓冲 |
| signaling WS buffer配置 | 20 KiB | ledger注释预期双向约40 KiB malloc缓冲；实际分配及生命周期需trace确认 |
| TLS record buffers | IN 16,384 / OUT 4,096 B | `MBEDTLS_EXTERNAL_MEM_ALLOC=y`，另有握手证书/密钥峰值；多连接不可只算一份 |
| LiveKit media stacks | aenc_0/AUD_SRC/Adec各40 KiB；peer pub/sub各25 KiB；buffer_in6 KiB；eng_stream4 KiB | media_lib_sal在IDF≥5使用SPIRAM WithCaps；capture stack_in_ext=true；TCB等仍非全部外移，运行时存在的任务集合待测 |
| LVGL draw buffer | 320×10×2 = **6,400 B** | CompanionLcdDisplay显式10行、单缓冲、内部DMA；不是320×240×2双全屏 |
| LVGL image cache | 上限2 MiB | 缓存预算，非启动即分配2 MiB；历史日志“Use 2MB”不是已消耗值 |
| LVGL simple draw layer | 24,576 B | Kconfig默认，只有相应渲染时才发生；全局layer max=0不提供硬上限 |
| Wi-Fi | static RX3、dynamic RX6、dynamic TX32 | 运行中按流量分配；已经较低的RX预算，不建议盲降 |

AFE已经使用 `AFE_MEMORY_ALLOC_MORE_PSRAM`、low-cost AEC，关闭AGC、设备AEC路径关闭VAD；不能把ESP-SR/DL整体删除而保持双工语音。AFE内部工作集有预编译实现，map只说明代码/静态数据，不能推算其分配峰值。

DMA统计应使用 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT` 明确要求。现有ledger的DMA分支只传 `MALLOC_CAP_DMA`，internal分支只传 `MALLOC_CAP_INTERNAL`；新测量应加与真实分配一致的cap组合，保留旧口径便于比较。ESP32-S3外部DMA是否支持还取决于外设、驱动、对齐和缓存一致性，不作“一切DMA均可/均不可在PSRAM”的概括。

cache关闭期间，外部Flash/PSRAM普通访问受限。OTA/NVS写入时不能贸然把会调用Flash服务的任务栈、ISR上下文、DMA descriptor、锁状态搬外部。SPI显示搬PSRAM可能触发内部bounce buffer，未必省内部峰值；当前10行缓冲已经节约，优先保留已修复显示路径。

### 4.3 历史实机数据与证据边界

只从 `/tmp/*box*.log` 提取 `[mem]` 与固件标识，不复制无关网络/身份日志。证据 `historical-memory.json` 包含源文件名、SHA256、行号、原始测量。**所有六份日志的ELF前缀都不同于当前 `b81aa939e`，以下仅为旧版本历史数据。**

| 日志/阶段 | internal free | largest block | free blocks | 说明 |
|---|---:|---:|---:|---|
| accept-box-fixed activation_done | 120,707 | 86,016 | 21 | min_free104,775；ELF b34d16f19 |
| 同日志首次 room_create前 | 56,103 | 55,296 | 2 | media board已建立，未包含完整连接峰值 |
| 同日志后一次 room_create前 | 56,247 | 29,696 | 6 | 总量近似恢复，但连续块减少25,600 B；提示碎片/布局改变，非泄漏定论 |
| accept-box-text activation_done | 120,675 | 86,016 | 21 | ELF931a40197 |
| 同日志 room_create前 | 55,883 | 55,296 | 3 | 与其它accept日志相近 |
| box3-recovery-live activation_done | 120,127 | 51,200 | 38 | min_free50,975；ELF8a4cd7ba1 |
| 同日志 room_create前 | 57,475 | 51,200 | 6 | 日志较晚采样；不能直接和冷启动min_free横比 |

accept-box-text activation_done的PSRAM free=16,764,088 B、largest=16,515,072 B；internal-DMA free=112,631 B。这说明当时PSRAM充裕，不说明说话/OTA峰值也如此。

旧日志栈未使用高水位（activation_done单阶段）：controller约14 KiB、activation约6–9 KiB、LVGL约2.8 KiB、main约4.1–5.4 KiB。这些是优先测量线索，**不是立即可回收值**。activation时未执行过的入会/重连/异常路径可能加深栈；ledger当前跳过了PSRAM栈逐任务明细，也不能据此证明其40 KiB栈过大。

| 阶段 | 当前版本证据 | 下一轮必须测量 |
|---|---|---|
| 冷启动/待机 | 缺实机数据 | UI完成、Wi-Fi连接、Owner恢复后各一次caps heap/stack |
| 入会 | 只有旧版pre-room | AFE前后、engine后、TLS/DTLS握手峰值、首音频后 |
| 双工说话/字幕/图标更新 | 无当前峰值 | 连续说话、长字幕分页、提示音并发、网络抖动；draw/FIFO/栈水位 |
| 重连/反复进出 | 旧版有连续块缩小 | 建议至少50轮，记录稳态free/largest、对象数与分配失败点 |
| OTA | 无数据 | 停会后下载/TLS、erase/write、校验、切槽、失败恢复；cache-disabled约束 |

每个阶段记录internal8bit、internalDMA、SPIRAM三组free/largest/minimum、任务栈位置/容量/高水位、分配失败caps和大小；用有界heap tracing追踪关键大块生命周期，避免诊断日志本身造成实时抖动。仅全局min_free不能按阶段归因，也不能看到一次大分配失败的largest约束。

## 5. 隔离实验：已测前后结果

脚本：`relink_experiment.py`。它通过 compile_commands 的原始参数重编译 **audio_encoder_reg.c、audio_decoder_reg.c**，在本地副本里 `#undef` 除 Opus/PCM 外的注册开关；复制并替换 `libespressif__esp_audio_codec.a` 的这两个member；其它对象（包括显示修复）保持原样。原sdkconfig与源文件均未改。这个实验与将BOX-3 profile配置收窄的目标相近，但不是已完成的Kconfig clean-build验证。

| 指标 | 原基线/无修改重链接 | Opus/PCM注册 | 差值 |
|---|---:|---:|---:|
| image B | 4,223,504 | 3,489,808 | -733,696 |
| OTA槽余量B | 36,336 | 770,032 | +733,696 |
| flash text B | 2,855,320 | 2,395,736 | -459,584 |
| flash rodata B | 1,233,584 | 966,448 | -267,136 |
| DRAM data B | 33,260 | 32,156 | -1,104 |
| DRAM bss B | 34,856 | 34,856 | 0 |
| IRAM text B | 99,903 | 94,043 | -5,860 |
| 静态D/IRAM B | 169,047 | 162,083 | -6,964 |

节省的image略不同于各section差值之和，包含布局/对齐变化。静态RAM释放不等于某一运行时最大块一定增加6,964 B，也不能据此宣称动态峰值已经降低同样数值。

结果：`experiments/results.json`；每组有 `commands.log`、ELF/bin/map和size/archives表。baseline bin哈希与审计开始时原件一致；候选bin SHA256=`f28f80decce01a49a55784b39b084c8e616f6902e7ed88c0f5bdca8ff2ac1a55`。两组产物均通过IDF `check_sizes.py` 分区检查，归档基线bin/ELF/map哈希校验通过，见 `evidence/validation.json`。候选产物仅用于离线审计。

业务依据：`LiveKitSession`明确publish Opus；`livekit_board.cc:274`/`:275`原先注册所有编码器和解码器，config中AAC、AMR、LC3、SBC、ALAC等均开启。提示音主路径 `AudioService::PlaySound`使用Ogg demux +直接Opus decoder，实验保留Opus及PCM。仍须验证远端协商/订阅编码、所有提示音和工具可触达音频格式；若需要G711等，只增加实际需要的格式重新测量，而非恢复全量注册。

没有测试“关闭AEC”“删CA”“缩PCM ring”等高风险方案；没有测量这些方案的收益，不给出伪造的前后数字。

## 6. 按收益、风险、工作量排序的建议

除首项外均为估算/目标，不可相加作为承诺收益。

| 优先级 / 方案 | 资源类型与收益 | 风险 / 工作量 | 建议 |
|---|---|---|---|
| P0 BOX-3 codec allowlist | **Flash+静态RAM；实测716.5 KiB+6.8 KiB** | 中低；配置/注册改动小，功能回归中 | 首选；正式profile限制format，clean build和实机完整语音链路验证 |
| P1 controller/activation栈 | RAM；目标约4–8 KiB/适用栈，尚未测得 | 中；测量与异常路径回归中 | 先补高水位与阶段数据，分次调整；不能把单次unused全部拿走 |
| P1 减少小音频帧vector分配/会话对象重复存活 | RAM/碎片；收益未量化 | 中；实现与回归中 | AFE输出有move后reserve、erase/copy循环；按固定帧池/生命周期实验，避免破坏UI代次过滤和关会等待 |
| P2 移除不需要的C++ stream/locale路径 | Flash+少量RAM；上界参考stdc++约210 KiB，实际预计为其子集 | 中；实现小至中、版本解析回归中 | `Ota::ParseVersion`使用stringstream/stoi；先测不改变解析语义的替代，保留OTA行为 |
| P2 BOX-3不构造无用large icon font | Flash；对象上界58,616 B，实际取决于其它引用 | 中低；小至中 | 先确认全部主题/提示/低电量/图标路径；保留20号图标及已修复布局 |
| P2 LVGL像素格式/默认主题/unused widgets裁剪 | 主要Flash；估计数KiB至数十KiB，未测 | 中；中 | 以符号可达性为依据；PNG/截图/字体fallback/字幕层回归 |
| P2 esp_network按能力拆MQTT等 | 主要Flash；当前EspMqtt对象约2.1 KiB是线索，非总收益 | 中；中 | 保留HTTPS/WebSocket/配网，别整删esp-ml307 |
| P2 renderer FIFO/PSRAM media stack | 主要PSRAM；可研究数十KiB级，未测 | 中高；中 | 与端到端延迟、抖动容忍、Opus复杂度联合测量；内部块紧张不等于该项最优先 |
| P2 assets引用清点/兼容清理 | 独立assets Flash；GIF上界4.49 MiB，未证明可全删 | 中；中 | 不改变app槽，先用引用统计证明未用；保留字幕中文覆盖、表情与回滚兼容 |
| P3 定制CA bundle/裁TLS算法 | 主要Flash；bundle上界68,987 B，不会全部可删 | 高；中高 | 涉及Hub/OTA/证书轮换/公共CA/DTLS；此轮不动信任与校验 |
| P3 常量/IRAM搬Flash或PSRAM | RAM换Flash执行开销/PSRAM带宽，具体未测 | 高；中高 | 缓存禁用、ISR及库只读假设验证后才考虑 |
| 不优先 PCM ring外移、减少Wi-Fi RX、缩TLS IN、扩OTA槽 | RAM或布局换性能/兼容 | 高 | PCM外移已有负面实机记录；其它都不是免费收益 |

仅省Flash的方案不会直接解决运行时碎片；仅减PSRAM不能帮助默认FreeRTOS内部队列；将IRAM函数放Flash可释放内部空间，但Flash容量并不一定减少（原IRAM指令也存在于image）。资源搬到assets还可能增加RAM缓存和版本管理成本。

## 7. 下一步与验收边界

建议保持现分区，先把allowlist变成BOX-3专属配置/注册实现，并保留其它板型配置。使用原未提交显示修复的完整源码快照做clean build，目标是复现约716 KiB节省；若差异超过对齐级别，重新解释对象/配置变化。

随后在获得刷机任务授权的后续工作中，验证配网、证书有效/无效、Owner恢复/重新认领、双OTA成功/失败回滚、Opus上下行、全部提示音、AEC打断/噪声、断网重连、固定双行分页字幕/图标/状态队列。同步采样第四节的分阶段内存，之后才选第二个RAM改动。当前任务授权范围内不刷机、不发布。

建议后续CI建立app尺寸余量门槛，例如至少256 KiB或槽容量5%（团队确认策略），并保存size/archive差分；不要只依赖“尚未超过槽大小”。这是建议策略，不是现有发布标准。

## 8. 复现命令与证据索引

版本库仅保存本报告、复现脚本、哈希清单、尺寸/分区/资源目录及实验结果。原始ELF/bin/map、完整配置、日志、源代码快照和链接命令仍保留在本机上述审计目录，由本目录 `.gitignore` 排除。下表是本地完整证据索引，不代表每个文件都随Git分发。新checkout不能脱离对应原构建输入直接重现实验；需取得匹配manifest的原构建目录及本地链接命令，或针对新源码重新建立基线。

以下路径按本次机器设置。命令为只读分析或在本报告目录内写实验结果；不要在原构建目录运行普通 `ninja`/`idf.py build`，那会修改原工作区。

```sh
# 原始链接归因；只读取map
/Users/manson/.espressif/python_env/idf5.5_py3.13_env/bin/python -m esp_idf_size --archives /Users/manson/ai/eidolon/eidolon-client-esp32/build/eidolon/esp-box-3/eidolon.map
/Users/manson/.espressif/python_env/idf5.5_py3.13_env/bin/python -m esp_idf_size --files /Users/manson/ai/eidolon/eidolon-client-esp32/build/eidolon/esp-box-3/eidolon.map
# 读取构建命令，不执行build
/Users/manson/.espressif/tools/ninja/1.12.1/ninja -C /Users/manson/ai/eidolon/eidolon-client-esp32/build/eidolon/esp-box-3 -t commands eidolon.elf
# 隔离重链接（需原构建对象与依赖仍可读；会覆盖本报告自己的experiments）
python3 /Users/manson/.codex/worktrees/0f1f/eidolon-client-esp32/reports/box3-size-memory-audit/relink_experiment.py
```

| 文件 | 用途 |
|---|---|
| `evidence/manifest.json` | 原产物哈希、字节、mtime |
| `evidence/source-head.txt`, `source-status.txt`, `source.patch`, `source-untracked/` | 未提交基线与来源 |
| `evidence/source/` | 本文关键源代码快照 |
| `evidence/sdkconfig.esp-box-3`, `sdkconfig.overlay.esp-box-3` | 实际配置；含项目配置值，仅作本地审计使用 |
| `evidence/dependencies.lock`, `idf_component.yml` | 固定依赖版本/解析来源 |
| `evidence/original.map`, `size.txt`, `archives.*`, `objects.txt` | 原始最终链接归因 |
| `evidence/symbols.txt`, `linked-symbol-names.txt`, `elf-sections.json` | 符号、section与存活判定 |
| `evidence/compiled-board-sources.txt` | 实际板级编译清单 |
| `evidence/partition-validation.json`, `16m_eidolon_box3.csv` | 二进制分区合同校验结果 |
| `evidence/assets.json` | 直接解析当前打包资源的目录与大小 |
| `evidence/historical-memory.json` | 历史日志的固件身份、原文件哈希、行号与内存测量 |
| `evidence/build.log`, `box3-display-experience.md` | 实际构建日志与显示行为约束 |
| `experiments/results.json`, 各变体`commands.log`/`size.txt`/`archives.txt` | 实测前后结果与全部命令 |

剩余不确定性：当前固件五阶段的实机峰值、最大连续internal8bit块、media栈高水位、远端所有允许音频格式、传统GIF的完整运行时消费者、OTA与共享assets版本兼容矩阵。报告中未将这些不确定项当作已经通过的验证。
