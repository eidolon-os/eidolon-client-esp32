#include "eidolon_lvgl_theme.h"

#include "lvgl_font.h"
#include "lvgl_theme.h"

#include <sdkconfig.h>

#include <lvgl.h>
#include <memory>

#if CONFIG_EIDOLON_HUB_MODE

// Tiny supplementary font: only the ~23 CJK glyphs the eidolon UI needs that the
// base subset font lacks (按住松开发送打断处理 etc.). It falls back to the base
// text font for every other glyph, so this adds ~4KB instead of swapping in the
// full multi-MB font.
LV_FONT_DECLARE(font_eidolon_ui_30);

namespace eidolon {

void RegisterEidolonThemes()
{
    auto text_font = LvglThemeManager::GetInstance().GetTheme("light");
    if (text_font == nullptr) {
        return;
    }

    auto light = LvglThemeManager::GetInstance().GetTheme("light");
    auto dark = LvglThemeManager::GetInstance().GetTheme("dark");
    if (light == nullptr || dark == nullptr) {
        return;
    }

    auto* eidolon_dark = new LvglTheme("eidolon_dark");
    eidolon_dark->set_background_color(lv_color_hex(0x0F1419));
    eidolon_dark->set_text_color(lv_color_hex(0xE6EDF3));
    eidolon_dark->set_chat_background_color(lv_color_hex(0x161B22));
    eidolon_dark->set_user_bubble_color(lv_color_hex(0x3B82F6));
    eidolon_dark->set_assistant_bubble_color(lv_color_hex(0x30363D));
    eidolon_dark->set_system_bubble_color(lv_color_hex(0x21262D));
    eidolon_dark->set_system_text_color(lv_color_hex(0xE6EDF3));
    eidolon_dark->set_border_color(lv_color_hex(0x30363D));
    eidolon_dark->set_low_battery_color(lv_color_hex(0xF85149));
    // Supplementary glyphs + runtime fallback to the base subset font. Copy the
    // const generated descriptor into a mutable static so we can point its
    // fallback at the active base font; the copied glyph/cmap pointers stay valid.
    static lv_font_t eidolon_text_font;
    static std::shared_ptr<LvglBuiltInFont> eidolon_text;
    if (light->text_font() && light->text_font()->font() != nullptr) {
        eidolon_text_font = font_eidolon_ui_30;
        eidolon_text_font.fallback = light->text_font()->font();
        eidolon_text = std::make_shared<LvglBuiltInFont>(&eidolon_text_font);
        eidolon_dark->set_text_font(eidolon_text);
    } else {
        eidolon_dark->set_text_font(light->text_font());
    }
    eidolon_dark->set_icon_font(light->icon_font());
    eidolon_dark->set_large_icon_font(light->large_icon_font());
    if (light->emoji_collection()) {
        eidolon_dark->set_emoji_collection(light->emoji_collection());
    }

    LvglThemeManager::GetInstance().RegisterTheme("eidolon_dark", eidolon_dark);
}

}  // namespace eidolon

#endif
