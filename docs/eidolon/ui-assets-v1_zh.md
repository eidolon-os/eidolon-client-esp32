# Eidolon UI 资源 v1

## 目标

第一版与默认小智皮肤区分：表情集、主题色、产品文案、启动图。

## 情绪名（勿改）

与 `SetEmotion()` / Agent 协议对齐：`neutral`, `happy`, `sad`, `angry`, `listening`, `thinking`, `sleepy` 等。

## 推荐尺寸

| 资源 | 尺寸 |
|------|------|
| 表情 PNG/GIF | 64×64 或 128×128 |
| 启动 Logo | 64×64 ~ 128×128 |

## 构建期替换（2.06 板）

- `main/CMakeLists.txt`：`DEFAULT_EMOJI_COLLECTION=noto-emoji_64`
- 主题：`eidolon_dark`（`main/eidolon/eidolon_lvgl_theme.cc`）
- 语言包：`main/assets/locales/zh-CN/language.json` 中 `EIDOLON_*`、`ROOM_*` 键

## OTA / 自定义 assets

1. 准备 `assets/eidolon-v1/index.json`（`text_font`, `emoji_collection[]`, `skin`）
2. 使用 `scripts/Image_Converter/lvgl_tools_gui.py` 或 `LVGLImage.py` 生成 `.bin`
3. 使用 `scripts/build_default_assets.py` 打包 `assets.bin` 烧录或 OTA

## 不落盘

聊天/转写历史不写入 NVS（见 [livekit-integration_zh.md](livekit-integration_zh.md) NVS 小节）。
