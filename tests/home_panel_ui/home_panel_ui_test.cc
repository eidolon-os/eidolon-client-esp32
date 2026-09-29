// korvo-1 smart home panel: the production HomePanelView, UI state projector
// and smart home store/wire compiled unchanged, driven by the SDK goldens, and
// drawn by LVGL's software renderer at 800x480. Only the ESP clock, logging,
// RNG and display lock are replaced. Writes one PPM per state.
#include <lvgl.h>
#include <font_awesome.h>
#include <cJSON.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cbin_font_fixture.h"
#include "display.h"
#include "eidolon/ui_state_mapper.h"
#include "eidolon/views/home_panel_copy.h"
#include "eidolon/views/home_panel_icons.h"
#include "eidolon/views/home_panel_view.h"

LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_home_icons_20);
LV_FONT_DECLARE(font_home_icons_24);
LV_FONT_DECLARE(font_home_icons_48);

using namespace eidolon;

namespace {

std::string g_out;
lv_point_t g_point;

// One UTF-8 code point at text[*i], advancing *i.
uint32_t NextCodePoint(const char* text, uint32_t* i) {
    const auto* s = reinterpret_cast<const unsigned char*>(text) + *i;
    uint32_t cp = *s;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp = extra == 3 ? cp & 0x07 : extra == 2 ? cp & 0x0F : extra == 1 ? cp & 0x1F : cp;
    ++*i;
    for (; extra > 0 && (s[1] & 0xC0) == 0x80; --extra, ++s, ++*i) cp = (cp << 6) | (s[1] & 0x3F);
    return cp;
}
bool g_down = false;
lv_indev_t* g_pointer = nullptr;

void Screenshot(lv_obj_t* screen, const std::string& name) {
    lv_obj_update_layout(screen);
    auto* shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    assert(shot);
    FILE* file = std::fopen((g_out + "/" + name + ".ppm").c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
    for (unsigned y = 0; y < shot->header.h; ++y) {
        for (unsigned x = 0; x < shot->header.w; ++x) {
            auto* p = shot->data + y * shot->header.stride + x * 3;
            const unsigned char rgb[] = {p[2], p[1], p[0]};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    lv_draw_buf_destroy(shot);
}

smarthome::Message Golden(const char* name) {
    std::ifstream file(std::string(SDK_GOLDEN_DIR) + "/" + name);
    assert(file);
    std::stringstream buffer;
    buffer << file.rdbuf();
    cJSON* root = cJSON_Parse(buffer.str().c_str());
    assert(root);
    const std::string op = cJSON_GetObjectItem(root, "op")->valuestring;
    char* payload = cJSON_PrintUnformatted(cJSON_GetObjectItem(root, "payload"));
    smarthome::Message message;
    std::string error;
    const bool ok = smarthome::ParseMessage(op, payload, message, &error);
    if (!ok) std::fprintf(stderr, "%s: %s\n", name, error.c_str());
    assert(ok);
    cJSON_free(payload);
    cJSON_Delete(root);
    return message;
}

void PointerRead(lv_indev_t*, lv_indev_data_t* data) {
    data->point = g_point;
    data->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void Press(int x, int y) {
    g_point = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
    g_down = true;
    lv_indev_read(g_pointer);
}
void Release() {
    g_down = false;
    lv_indev_read(g_pointer);
}
void ClickAt(int x, int y) {
    Press(x, y);
    Release();
}

bool Shown(lv_obj_t* obj) {
    for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

lv_obj_t* Find(lv_obj_t* obj, const std::string& text, lv_obj_t* after = nullptr, bool* passed = nullptr) {
    bool local = after == nullptr;
    if (passed == nullptr) passed = &local;
    if (lv_obj_check_type(obj, &lv_label_class) && Shown(obj) && text == lv_label_get_text(obj)) {
        if (*passed) return obj;
        if (obj == after) *passed = true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (auto* found = Find(lv_obj_get_child(obj, i), text, after, passed)) return found;
    }
    return nullptr;
}

// The touchable label showing `text` (the status line can show the same icon).
lv_obj_t* FindTouchable(lv_obj_t* obj, const char* text) {
    if (lv_obj_check_type(obj, &lv_label_class) && Shown(obj) && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        std::strcmp(lv_label_get_text(obj), text) == 0)
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (auto* found = FindTouchable(lv_obj_get_child(obj, i), text)) return found;
    }
    return nullptr;
}

lv_area_t Coords(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

void ClickObject(lv_obj_t* obj) {
    const lv_area_t a = Coords(obj);
    ClickAt((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

// A tile, tapped away from its - / + buttons.
void ClickTile(lv_obj_t* screen, const char* name) {
    lv_obj_t* label = Find(screen, name);
    assert(label);
    const lv_area_t tile = Coords(lv_obj_get_parent(label));
    ClickAt(tile.x1 + 16, tile.y2 - 12);
}

// Every character a visible label shows must draw in that label's font chain.
void AssertGlyphs(lv_obj_t* obj, const char* scenario) {
    if (lv_obj_check_type(obj, &lv_label_class) && Shown(obj)) {
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        const char* text = lv_label_get_text(obj);
        for (uint32_t i = 0; text[i] != '\0';) {
            const uint32_t cp = NextCodePoint(text, &i);
            lv_font_glyph_dsc_t dsc;
            if (cp <= 0x20 || cp == 0x3000) continue;
            if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                std::fprintf(stderr, "%s: U+%04X missing in \"%s\"\n", scenario, static_cast<unsigned>(cp), text);
                assert(false);
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) AssertGlyphs(lv_obj_get_child(obj, i), scenario);
}

// No visible descendant spills out of its parent: labels that outgrow their
// box are truncated, never drawn over a neighbour.
void AssertContained(lv_obj_t* obj) {
    const lv_area_t outer = Coords(obj);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (!Shown(child)) continue;
        const lv_area_t inner = Coords(child);
        if (inner.x1 < outer.x1 || inner.y1 < outer.y1 || inner.x2 > outer.x2 || inner.y2 > outer.y2) {
            std::fprintf(stderr, "child (%d,%d)-(%d,%d) outside (%d,%d)-(%d,%d)\n", (int)inner.x1, (int)inner.y1,
                         (int)inner.x2, (int)inner.y2, (int)outer.x1, (int)outer.y1, (int)outer.x2, (int)outer.y2);
            assert(false);
        }
    }
}

bool NavSelected(lv_obj_t* screen, const char* area) {
    lv_obj_t* item = Find(screen, area);
    assert(item);
    return lv_obj_has_state(item, LV_STATE_CHECKED);
}

class RecordingSink : public smarthome::PanelSink {
public:
    void SendRequest(const std::string& request_json) override { requests.push_back(request_json); }
    bool Last(std::initializer_list<const char*> parts) const {
        if (requests.empty()) return false;
        for (const char* part : parts) {
            if (requests.back().find(part) == std::string::npos) return false;
        }
        return true;
    }
    std::vector<std::string> requests;
};

UiInputProfile KorvoInputs() {
    UiInputProfile inputs;
    inputs.enabled_inputs = InputBit(UiInputSource::Touch) | InputBit(UiInputSource::SessionButton);
    inputs.available_inputs = inputs.enabled_inputs;
    inputs.setup_available = true;
    inputs.microphone_mute = false;  // CONFIG_EIDOLON_UI_MICROPHONE_MUTE=n on korvo-1
    return inputs;
}

EidolonRuntimeStatus Ready() {
    EidolonRuntimeStatus s;
    s.runtime = RuntimePhase::Normal;
    s.enrollment = EnrollmentPhase::ClaimActive;
    s.service = ServicePhase::Ready;
    s.interaction_mode = InteractionMode::HalfDuplex;
    return s;
}

bool HasChinese(const std::string& text) {
    for (uint32_t i = 0; i < text.size();) {
        if (NextCodePoint(text.c_str(), &i) >= 0x2E80) return true;
    }
    return false;
}

lv_obj_t* NewScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);
    return screen;
}

// Every glyph the view can ask for is in the font that draws it.
void TestIconFonts() {
    lv_font_glyph_dsc_t dsc;
    const auto has = [&](const lv_font_t* font, const char* utf8) {
        uint32_t i = 0;
        return lv_font_get_glyph_dsc(font, &dsc, NextCodePoint(utf8, &i), 0);
    };
    for (int i = 0; i <= static_cast<int>(smarthome::DeviceIcon::Thermometer); ++i) {
        assert(has(&font_home_icons_24, home_icons::Device(static_cast<smarthome::DeviceIcon>(i))));
    }
    for (int i = 0; i <= static_cast<int>(UiScene::Error); ++i) {
        assert(has(&font_home_icons_48, home_icons::Hero(static_cast<UiScene>(i))));
    }
    for (const char* icon : {HOME_ICON_MIC, HOME_ICON_SPARKLE, HOME_ICON_SYNC, HOME_ICON_CHECK_CIRCLE,
                             HOME_ICON_ERROR, HOME_ICON_WARNING, HOME_ICON_QUESTION}) {
        assert(has(&font_home_icons_24, icon));
    }
    for (const char* icon : {HOME_ICON_ADD, HOME_ICON_REMOVE, HOME_ICON_CHEVRON_LEFT, HOME_ICON_CHEVRON_RIGHT,
                             HOME_ICON_SETTINGS, HOME_ICON_SYNC, HOME_ICON_CLOUD_OFF}) {
        assert(has(&font_home_icons_20, icon));
    }
}

// What the projector and the Application say on the system page reads in
// Chinese; the gesture it names is korvo-1's own (SET, not BOOT).
void TestCopy() {
    const UiInputProfile inputs = KorvoInputs();
    const auto check = [&](const EidolonRuntimeStatus& s) {
        const EidolonUiModel model = UiStateProjector::Project(s, inputs);
        const std::string title = home_copy::Title(model.scene, model.status_text);
        const std::string detail = home_copy::Text(model.detail_text);
        if (!HasChinese(title) || !HasChinese(detail)) {
            std::fprintf(stderr, "untranslated: \"%s\" / \"%s\"\n", model.status_text, model.detail_text);
            assert(false);
        }
    };
    for (int r = 0; r <= static_cast<int>(RuntimePhase::Fault); ++r) {
        EidolonRuntimeStatus s;
        s.runtime = static_cast<RuntimePhase>(r);
        check(s);
    }
    for (int e = 0; e <= static_cast<int>(EnrollmentPhase::Revoked); ++e) {
        for (int v = 0; v <= static_cast<int>(ServicePhase::Fault); ++v) {
            EidolonRuntimeStatus s = Ready();
            s.enrollment = static_cast<EnrollmentPhase>(e);
            s.service = static_cast<ServicePhase>(v);
            const UiScene scene = UiStateProjector::Project(s, inputs).scene;
            if (scene != UiScene::Ready) check(s);
        }
    }
    for (const char* detail : {
             "Preparing secure device setup...", "Ready for secure device setup",
             "Configuration recovery blocked. Hold BOOT to retry setup", "Validating Wi-Fi and Host...",
             "Wi-Fi and Host confirmed", "Finishing device setup...",
             "Returning to saved Wi-Fi. Hold BOOT to change network", "Setup closed - long-press BOOT to reopen",
             "Wi-Fi disconnected", "Owner network unavailable", "Downloading UI assets...",
             "Update complete. Restarting...", "Firmware update failed", "Restoring the operational channel...",
             "Connecting to Wi-Fi Eidolon-Home...", "Installing firmware 3.3.0", "Press button to start",
             "Press button to end"}) {
        const std::string text = home_copy::Text(detail);
        assert(HasChinese(text));
        assert(text.find("BOOT") == std::string::npos);
    }
    assert(home_copy::Text("Connecting to Wi-Fi Eidolon-Home...") == "正在连接 Wi-Fi「Eidolon-Home」");
    assert(home_copy::Text("A detail the Hub sent") == "A detail the Hub sent");
    assert(home_copy::ProgressPercent("37% 512KB/s") == 37);
    assert(home_copy::ProgressPercent("Installing firmware") == -1);
}

// System pages as they draw before the assets partition is loaded: the builtin
// 206-character subset plus the panel's own supplement.
void TestBootFontSystemPages(Display& display) {
    lv_obj_t* screen = NewScreen();
    auto view = std::make_unique<HomePanelView>();
    view->Build({screen, &display, &font_puhui_basic_20_4, &font_awesome_20_4});
    const UiInputProfile inputs = KorvoInputs();
    for (int r = 0; r <= static_cast<int>(RuntimePhase::Fault); ++r) {
        EidolonRuntimeStatus s;
        s.runtime = static_cast<RuntimePhase>(r);
        if (s.runtime == RuntimePhase::Normal) continue;
        view->Render(UiStateProjector::Project(s, inputs));
        AssertGlyphs(screen, "boot-font system page");
    }
    for (int v = 0; v <= static_cast<int>(ServicePhase::Fault); ++v) {
        EidolonRuntimeStatus s = Ready();
        s.service = static_cast<ServicePhase>(v);
        const EidolonUiModel model = UiStateProjector::Project(s, inputs);
        if (model.scene == UiScene::Ready) continue;  // the dashboard needs the Host, so the assets
        view->Render(model);
        AssertGlyphs(screen, "boot-font service page");
    }
    view.reset();  // deletes its own objects first
    lv_obj_delete(screen);
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    g_out = argv[1];
    lv_init();
    auto* disp = lv_display_create(800, 480);
    static uint8_t pixels[800 * 40 * 2];
    lv_display_set_buffers(disp, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    g_pointer = lv_indev_create();
    lv_indev_set_type(g_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_pointer, PointerRead);
    std::vector<UiIntent> intents;
    SetEidolonUiIntentHandler([&](UiIntent intent) { intents.push_back(intent); });

    TestIconFonts();
    TestCopy();
    Display display;
    TestBootFontSystemPages(display);

    // The assets partition's full-coverage text font, as the panel runs.
    CbinFontFixture common(CBIN_FONT_FILE);
    lv_obj_t* screen = NewScreen();
    HomePanelView view;
    RecordingSink sink;
    view.SetSink(&sink);
    view.Build({screen, &display, common.font(), &font_awesome_20_4});
    view.SetNetworkIcon(FONT_AWESOME_WIFI);
    const UiInputProfile inputs = KorvoInputs();
    const auto render = [&](const EidolonRuntimeStatus& s) {
        view.Render(UiStateProjector::Project(s, inputs));
    };
    // The microphone is the session control: its tap must be exactly what a
    // click of SET or MODE (the session button) resolves to, in this state.
    const auto mic_matches_session_button = [&](const EidolonRuntimeStatus& s) {
        const UiIntent button =
            UiStateProjector::ResolveInput(s, inputs, UiInputSource::SessionButton, UiInputGesture::Click);
        lv_obj_t* mic = nullptr;
        for (const char* icon : {HOME_ICON_MIC, HOME_ICON_SYNC, HOME_ICON_SPARKLE}) {
            if (mic == nullptr) mic = FindTouchable(screen, icon);
        }
        assert(mic);
        intents.clear();
        ClickObject(mic);
        assert(intents.size() == (button == UiIntent::None ? 0u : 1u));
        if (button != UiIntent::None) assert(intents.back() == button);
        return button;
    };
    const auto shot = [&](const char* name) {
        AssertGlyphs(screen, name);
        Screenshot(screen, name);
    };

    // --- System pages --------------------------------------------------------
    {
        EidolonRuntimeStatus s;
        s.runtime = RuntimePhase::Booting;
        render(s);
        shot("sys-01-starting");
        s.runtime = RuntimePhase::NetworkConnecting;
        s.runtime_detail = "Connecting to Wi-Fi Eidolon-Home...";
        render(s);
        shot("sys-02-network");
        s.runtime = RuntimePhase::Commissioning;
        s.runtime_detail = "Ready for secure device setup";
        render(s);
        shot("sys-03-commissioning");
        assert(Find(screen, std::string(HOME_ICON_SETTINGS "  ") + home_copy::kSetupAction));
        s.runtime_detail = "Setup closed - long-press BOOT to reopen";
        render(s);
        assert(Find(screen, "配网已关闭，长按 SET 键重新打开"));
        shot("sys-04-setup-closed");
        s = Ready();
        s.enrollment = EnrollmentPhase::PendingReview;
        render(s);
        shot("sys-05-waiting-approval");
        s = Ready();
        s.service = ServicePhase::DiscoveringAuthority;
        render(s);
        assert(Find(screen, "正在寻找主机"));
        shot("sys-06-finding-service");
        s = Ready();
        s.service = ServicePhase::Unreachable;
        render(s);
        shot("sys-07-host-unreachable");
        s = EidolonRuntimeStatus{};
        s.runtime = RuntimePhase::Updating;
        s.runtime_detail = "37% 512KB/s";
        render(s);
        shot("sys-08-updating");
        s.runtime = RuntimePhase::Fault;
        s.runtime_detail = "Firmware update failed";
        render(s);
        shot("sys-09-fault");
        // The setup button asks for setup, whatever the page.
        intents.clear();
        ClickObject(Find(screen, std::string(HOME_ICON_SETTINGS "  ") + home_copy::kSetupAction));
        assert(intents.size() == 1 && intents.back() == UiIntent::OpenSetup);
    }

    // --- Dashboard -----------------------------------------------------------
    // 2026-09-29 20:31 in the home's +08:00.
    const int64_t kNow = 1790685060000LL;
    const EidolonRuntimeStatus ready = Ready();
    render(ready);
    view.SetLinkUp(true);
    assert(sink.Last({"smarthome.sync"}));
    shot("dash-00-no-snapshot");

    smarthome::Message snapshot = Golden("panel-snapshot.json");
    const smarthome::Snapshot home = snapshot.snapshot;
    view.Apply(std::move(snapshot));
    view.SetWallClock(kNow);
    render(ready);
    // The panel opens on its own area (living) even though it was built, and
    // drew its navigation, before any snapshot existed.
    assert(NavSelected(screen, "客厅") && !NavSelected(screen, "全部"));
    assert(Find(screen, "20:31"));
    // No scene buttons: the status line has the bar, and says how to speak
    // until something has happened.
    assert(!Find(screen, "回家") && Find(screen, "点麦克风或按 SET，说出指令"));
    shot("dash-01-living");

    view.Apply(Golden("panel-delta.json"));
    render(ready);
    assert(Find(screen, "客厅空调 已打开") && Find(screen, "20:31 · 来自 面板语音"));
    shot("dash-02-living-after-delta");

    // Layout: tiles and their parts stay inside the grid and their tile.
    for (const char* name : {"客厅主灯", "空气净化器", "温湿度计"}) {
        lv_obj_t* tile = lv_obj_get_parent(Find(screen, name));
        AssertContained(tile);
        const lv_area_t a = Coords(tile);
        assert(a.x1 >= 120 && a.y1 >= 48 && a.x2 < 800 && a.y2 < 480 - 68);
    }
    {
        lv_obj_t* mic = FindTouchable(screen, HOME_ICON_MIC);
        const lv_area_t a = Coords(mic);
        assert(a.x2 - a.x1 + 1 >= 52 && a.y1 >= 480 - 68);
        AssertContained(lv_obj_get_parent(mic));
    }

    // Touch still sends what it always did, and only through the sink.
    sink.requests.clear();
    ClickTile(screen, "客厅主灯");
    assert(sink.Last({"smarthome.execute", "living.main_light", "on_off", "toggle"}));
    {
        lv_obj_t* tile = lv_obj_get_parent(Find(screen, "客厅主灯"));
        lv_obj_t* plus = nullptr;
        for (uint32_t i = 0; i < lv_obj_get_child_count(tile); ++i) {
            lv_obj_t* child = lv_obj_get_child(tile, i);
            if (lv_obj_check_type(child, &lv_label_class) && std::strcmp(lv_label_get_text(child), HOME_ICON_ADD) == 0)
                plus = child;
        }
        assert(plus);
        ClickObject(plus);
        assert(sink.Last({"living.main_light", "\"level\"", "\"step\"", "20"}));
    }
    const size_t sent = sink.requests.size();
    shot("dash-10-living-idle");

    // A picked area is kept across snapshots; the panel's own area returns
    // when the picked one is gone, and follows the panel when it is moved.
    ClickObject(Find(screen, "主卧"));
    assert(NavSelected(screen, "主卧"));
    smarthome::Message again;
    again.kind = smarthome::MessageKind::Snapshot;
    again.snapshot = home;
    again.snapshot.revision += 1;
    view.Apply(std::move(again));
    assert(NavSelected(screen, "主卧"));
    smarthome::Message without;
    without.kind = smarthome::MessageKind::Snapshot;
    without.snapshot = home;
    without.snapshot.revision += 2;
    for (auto& device : without.snapshot.devices) {
        if (device.area_id == "master") device.area_id = "living";
    }
    view.Apply(std::move(without));
    assert(NavSelected(screen, "客厅"));
    smarthome::Message moved;
    moved.kind = smarthome::MessageKind::Snapshot;
    moved.snapshot = home;
    moved.snapshot.revision += 3;
    moved.snapshot.panel_area_id = "master";
    view.Apply(std::move(moved));
    assert(NavSelected(screen, "主卧"));
    smarthome::Message back;
    back.kind = smarthome::MessageKind::Snapshot;
    back.snapshot = home;  // the golden revision again, so the golden delta applies
    view.Apply(std::move(back));
    view.Apply(Golden("panel-delta.json"));
    assert(NavSelected(screen, "客厅"));

    // "全部": 18 devices, two pages.
    ClickObject(Find(screen, "全部"));
    render(ready);
    assert(Find(screen, "1 / 2"));
    shot("dash-03-all-page1");
    ClickObject(Find(screen, HOME_ICON_CHEVRON_RIGHT));
    assert(Find(screen, "2 / 2") && Find(screen, "温湿度计"));
    ClickObject(Find(screen, HOME_ICON_CHEVRON_LEFT));
    assert(Find(screen, "1 / 2"));
    ClickObject(Find(screen, "客厅"));
    render(ready);
    assert(sink.requests.size() == sent);

    // Pressed feedback, held and released.
    for (const auto& [name, file] : {std::pair<const char*, const char*>{"客厅主灯", "dash-08-pressed-on"},
                                     std::pair<const char*, const char*>{"电视", "dash-09-pressed-off"}}) {
        const lv_area_t tile = Coords(lv_obj_get_parent(Find(screen, name)));
        Press(tile.x1 + 16, tile.y2 - 12);
        assert(lv_obj_has_state(lv_obj_get_parent(Find(screen, name)), LV_STATE_PRESSED));
        Screenshot(screen, file);
        Release();
    }
    sink.requests.clear();

    // --- Voice ---------------------------------------------------------------
    assert(mic_matches_session_button(ready) == UiIntent::OpenConversation);

    EidolonRuntimeStatus voice = ready;
    voice.conversation = ConversationPhase::Opening;
    render(voice);
    assert(Find(screen, "正在连接…") && Find(screen, HOME_ICON_SYNC) && Find(screen, "点麦克风或按 SET 取消"));
    assert(mic_matches_session_button(voice) == UiIntent::CloseConversation);
    shot("voice-00-opening");

    voice.conversation = ConversationPhase::Active;
    voice.turn = TurnPhase::Idle;
    render(voice);
    assert(Find(screen, "请说出指令") && Find(screen, "点麦克风或按 SET 结束"));
    shot("voice-01-idle-listening");
    // Ends the conversation, as SET does; it no longer only mutes.
    assert(mic_matches_session_button(voice) == UiIntent::CloseConversation);

    voice.turn = TurnPhase::UserSpeaking;
    render(voice);
    assert(Find(screen, "正在聆听…"));
    assert(mic_matches_session_button(voice) == UiIntent::CloseConversation);
    shot("voice-02-user-speaking");

    voice.turn = TurnPhase::AgentThinking;
    voice.last_transcription = "打开空调";
    voice.last_transcription_role = "user";
    render(voice);
    assert(Find(screen, "「打开空调」") && Find(screen, "正在理解…"));
    assert(mic_matches_session_button(voice) == UiIntent::CloseConversation);
    shot("voice-03-processing");

    view.Apply(Golden("voice-result-executed.json"));
    voice.turn = TurnPhase::Idle;
    render(voice);
    // The result is the status line's, over nothing: no tray without a choice.
    assert(Find(screen, HOME_ICON_CHECK_CIRCLE) && Find(screen, "「打开空调」"));
    assert(Coords(Find(screen, "已打开客厅空调 · 26°C")).y1 >= 480 - 68);
    assert(!Find(screen, "客厅空调", Find(screen, "客厅空调")));  // only the tile carries the name
    shot("voice-04-result-executed");

    view.Apply(Golden("voice-result-ambiguous.json"));
    render(voice);
    assert(Find(screen, "要打开哪台空调？") && Find(screen, HOME_ICON_QUESTION));
    {
        // The tray of choices sits over the grid: never over the area
        // navigation, never over the status line that asks the question.
        const lv_area_t tray = Coords(lv_obj_get_parent(Find(screen, "主卧空调")));
        assert(tray.x1 >= 120 && tray.y2 < 480 - 68);
    }
    shot("voice-05-result-ambiguous");
    ClickObject(Find(screen, "主卧空调"));
    assert(sink.Last({"smarthome.execute", "master.ac", "on_off", "\"on\""}));
    assert(!Find(screen, "要打开哪台空调？"));

    {
        smarthome::Message m = Golden("voice-result-executed.json");
        m.result.utterance = "打开投影仪";
        m.result.outcome = smarthome::VoiceOutcome::NotFound;
        m.result.message = "家里没有投影仪";
        view.Apply(std::move(m));
        render(voice);
        assert(Find(screen, HOME_ICON_ERROR));
        shot("voice-06-result-not-found");
    }

    // The panel has no mute: once the result has run out the line is back to
    // the conversation itself, and a stored "muted" never shows or silences.
    lv_tick_inc(7000);
    lv_timer_handler();
    voice.mic_enabled = false;
    render(voice);
    assert(Find(screen, "请说出指令") && !Find(screen, "麦克风已关闭"));
    assert(mic_matches_session_button(voice) == UiIntent::CloseConversation);
    voice.mic_enabled = true;

    // --- Toast, stale, empty -------------------------------------------------
    render(ready);
    assert(!Find(screen, "家里没有投影仪"));
    view.ShowNotification("音量 60", 3000);
    assert(Coords(Find(screen, "音量 60")).y2 < 48);
    shot("dash-04-toast");
    view.ShowNotification("", 500);

    EidolonRuntimeStatus reconnecting = ready;
    reconnecting.service = ServicePhase::Reconnecting;
    render(reconnecting);
    assert(mic_matches_session_button(reconnecting) == UiIntent::None);  // nothing to start or end
    shot("dash-05-reconnecting-stale");

    render(ready);
    view.SetLinkUp(false);
    render(ready);
    assert(Find(screen, HOME_ICON_CLOUD_OFF " 离线 · 显示上次状态"));
    shot("dash-06-link-down-stale");
    sink.requests.clear();
    ClickTile(screen, "客厅主灯");
    assert(sink.requests.empty());  // a stale cache is inert
    view.SetLinkUp(true);

    {
        smarthome::Message empty;
        empty.kind = smarthome::MessageKind::Snapshot;
        empty.snapshot = home;
        empty.snapshot.revision += 10;
        empty.snapshot.devices.clear();
        empty.snapshot.scenes.clear();
        view.Apply(std::move(empty));
        render(ready);
        shot("dash-07-empty-home");
    }

    std::printf("home_panel_ui_tests: all passed (%s)\n", g_out.c_str());
    return 0;
}
