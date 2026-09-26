#include "eidolon/views/home_panel_view.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <font_awesome.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#include "display.h"

namespace eidolon {

namespace {

constexpr const char* kTag = "HomePanel";

// 800x480 layout (the reference mockup's .stage): 40 px top bar, 112 px area
// navigation, 4 x 3 tiles, 64 px bottom bar.
constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr int kTopH = 40;
constexpr int kBottomH = 64;
constexpr int kNavW = 112;
constexpr int kMidH = kHeight - kTopH - kBottomH;
constexpr int kGridW = kWidth - kNavW;
constexpr int kGridPad = 12;
constexpr int kGap = 10;
constexpr int kCols = 4;
constexpr int kRows = 3;
constexpr int kTileW = (kGridW - 2 * kGridPad - (kCols - 1) * kGap) / kCols;
constexpr int kTileH = 100;
constexpr int kTilePad = 8;
constexpr int kStepW = 36;
constexpr int kPagerY = kGridPad + kRows * kTileH + (kRows - 1) * kGap + 6;
constexpr int kPagerH = 32;
constexpr int kCardMargin = 44;
constexpr int kResultMs = 6000;
constexpr int kChoiceResultMs = 12000;

namespace color {
constexpr uint32_t Canvas = 0x0D1015;
constexpr uint32_t Rule = 0x1D232D;
constexpr uint32_t Ink = 0xE8ECF2;
constexpr uint32_t Muted = 0x8A94A6;
constexpr uint32_t Clock = 0xC9D0DB;
constexpr uint32_t Conn = 0x9FB0C4;
constexpr uint32_t Online = 0x42C47A;
constexpr uint32_t Offline = 0x6B7482;
constexpr uint32_t Control = 0x232A35;
constexpr uint32_t ControlInk = 0xDCE2EA;
constexpr uint32_t ControlPressed = 0x2D3542;
constexpr uint32_t Amber = 0xF4A73B;
constexpr uint32_t AmberInk = 0x2A1A04;
constexpr uint32_t Busy = 0xA78BFA;
constexpr uint32_t Nav = 0xAAB3C2;
constexpr uint32_t NavOn = 0x1C232E;
constexpr uint32_t Count = 0x6E7888;
constexpr uint32_t Tile = 0x171C24;
constexpr uint32_t TileEdge = 0x232A35;
constexpr uint32_t TileOn = 0x241D12;
constexpr uint32_t TileOnEdge = 0x5A4217;
constexpr uint32_t TilePressed = 0x1F2630;
constexpr uint32_t Glyph = 0x222A36;
constexpr uint32_t GlyphInk = 0x9AA6B8;
constexpr uint32_t State = 0x9AA4B5;
constexpr uint32_t StateOn = 0xF6C27A;
constexpr uint32_t Scene = 0x1A2029;
constexpr uint32_t SceneEdge = 0x28303C;
constexpr uint32_t ScenePressed = 0x222A35;
constexpr uint32_t Card = 0x1E2530;
constexpr uint32_t CardEdge = 0x3A4656;
constexpr uint32_t Message = 0xF2F4F8;
constexpr uint32_t MessageError = 0xFF9A90;
constexpr uint32_t Toast = 0x2C5FB8;
constexpr uint32_t Attention = 0xF5B84B;
constexpr uint32_t Error = 0xFF6B6B;
}  // namespace color

// Shared styles: one lv_style_t per look instead of local style arrays on
// every object, since small LVGL allocations land in internal SRAM here.
struct Styles {
    lv_style_t root, bar, muted, clock, conn, dot, dot_on, mic, mic_live, mic_busy;
    lv_style_t nav, nav_on, count, tile, tile_on, tile_pressed, glyph, glyph_on, name, state, state_on;
    lv_style_t step, step_pressed, pager, dim, scene, scene_pressed, activity;
    lv_style_t card, heard, message, message_attention, message_error, choice, toast;
    lv_style_t sys_title, sys_attention, sys_error, sys_detail, sys_action;
    bool ready = false;
};

Styles& S() {
    static Styles styles;
    return styles;
}

int64_t MonoMs() { return esp_timer_get_time() / 1000; }

// Role and slot index of a touchable object, carried in its user_data.
void* Tag(uint8_t role, size_t index) {
    return reinterpret_cast<void*>((static_cast<uintptr_t>(role) << 16) | (index & 0xFFFF));
}
uint8_t TagRole(const void* tag) { return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(tag) >> 16); }
size_t TagIndex(const void* tag) { return reinterpret_cast<uintptr_t>(tag) & 0xFFFF; }

lv_obj_t* NewBox(lv_obj_t* parent, lv_style_t* style = nullptr) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    if (style != nullptr) lv_obj_add_style(obj, style, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    return obj;
}

lv_obj_t* NewLabel(lv_obj_t* parent, lv_style_t* style = nullptr) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    if (style != nullptr) lv_obj_add_style(label, style, 0);
    lv_label_set_text_static(label, "");
    return label;
}

void Touchable(lv_obj_t* obj, void* tag) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(obj, tag);
}

void Visible(lv_obj_t* obj, bool visible) {
    if (obj == nullptr || visible == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, !visible);
}

// Invalidate only on a real change: the RGB panel runs full refreshes, so
// every needless invalidation repaints all 800x480 pixels.
void Text(lv_obj_t* label, const char* text) {
    const char* value = text != nullptr ? text : "";
    if (std::strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}
void Text(lv_obj_t* label, const std::string& text) { Text(label, text.c_str()); }

void SetState(lv_obj_t* obj, lv_state_t state, bool on) {
    if (lv_obj_has_state(obj, state) != on) lv_obj_set_state(obj, state, on);
}

int PadFor(int box, int line) { return std::max(0, (box - line) / 2); }

bool IsTalking(TurnPhase turn) { return turn == TurnPhase::Recording || turn == TurnPhase::UserSpeaking; }
bool IsBusy(TurnPhase turn) { return turn == TurnPhase::Committing || turn == TurnPhase::AgentThinking; }

// Decodes one UTF-8 code point; returns 0 at the end of the string.
uint32_t NextCodePoint(const char*& p) {
    const auto* s = reinterpret_cast<const unsigned char*>(p);
    if (*s == 0) return 0;
    uint32_t cp = *s;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp = extra == 3 ? cp & 0x07 : extra == 2 ? cp & 0x0F : extra == 1 ? cp & 0x1F : cp;
    ++s;
    for (; extra > 0 && (*s & 0xC0) == 0x80; --extra, ++s) cp = (cp << 6) | (*s & 0x3F);
    p = reinterpret_cast<const char*>(s);
    return cp;
}

// The display lock, when there is a display yet: surface calls may arrive
// before Build(), when there is nothing to draw and nothing to race with.
class ViewLock {
public:
    explicit ViewLock(Display* display) {
        if (display != nullptr) guard_.emplace(display);
    }

private:
    std::optional<DisplayLockGuard> guard_;
};

void TextStatic(lv_obj_t* label, const char* literal) {
    if (std::strcmp(lv_label_get_text(label), literal) != 0) lv_label_set_text_static(label, literal);
}

}  // namespace

void HomePanelView::LoggingSink::SendRequest(const std::string& request_json) {
    ESP_LOGI(kTag, "[smarthome] %s (no transport yet): %s", smarthome::kPanelRequestTopic, request_json.c_str());
}

HomePanelView::HomePanelView() : request_ids_(esp_random()) {}

HomePanelView::~HomePanelView() {
    if (timer_ != nullptr) lv_timer_delete(timer_);
    if (root_ != nullptr) lv_obj_delete(root_);
}

// --- Build -------------------------------------------------------------------

void HomePanelView::BuildStyles() {
    Styles& s = S();
    if (s.ready) return;
    s.ready = true;
    const auto init = [](lv_style_t& style) { lv_style_init(&style); };
    const auto fill = [](lv_style_t& style, uint32_t bg) {
        lv_style_set_bg_color(&style, lv_color_hex(bg));
        lv_style_set_bg_opa(&style, LV_OPA_COVER);
    };
    const auto ink = [](lv_style_t& style, uint32_t text) { lv_style_set_text_color(&style, lv_color_hex(text)); };

    init(s.root);
    fill(s.root, color::Canvas);
    ink(s.root, color::Ink);
    init(s.bar);
    lv_style_set_border_color(&s.bar, lv_color_hex(color::Rule));
    lv_style_set_border_width(&s.bar, 1);
    init(s.muted);
    ink(s.muted, color::Muted);
    init(s.clock);
    ink(s.clock, color::Clock);
    init(s.conn);
    ink(s.conn, color::Conn);
    init(s.dot);
    lv_style_set_radius(&s.dot, LV_RADIUS_CIRCLE);
    fill(s.dot, color::Offline);
    init(s.dot_on);
    lv_style_set_bg_color(&s.dot_on, lv_color_hex(color::Online));
    init(s.mic);
    lv_style_set_radius(&s.mic, LV_RADIUS_CIRCLE);
    fill(s.mic, color::Control);
    ink(s.mic, color::ControlInk);
    lv_style_set_text_align(&s.mic, LV_TEXT_ALIGN_CENTER);
    init(s.mic_live);
    lv_style_set_bg_color(&s.mic_live, lv_color_hex(color::Amber));
    ink(s.mic_live, color::AmberInk);
    init(s.mic_busy);
    lv_style_set_bg_color(&s.mic_busy, lv_color_hex(color::Busy));
    ink(s.mic_busy, color::AmberInk);

    init(s.nav);
    lv_style_set_radius(&s.nav, 8);
    lv_style_set_pad_left(&s.nav, 12);
    lv_style_set_pad_right(&s.nav, 30);  // room for the device count
    ink(s.nav, color::Nav);
    init(s.nav_on);
    fill(s.nav_on, color::NavOn);
    ink(s.nav_on, 0xFFFFFF);
    init(s.count);
    ink(s.count, color::Count);

    init(s.tile);
    fill(s.tile, color::Tile);
    lv_style_set_radius(&s.tile, 12);
    lv_style_set_border_width(&s.tile, 1);
    lv_style_set_border_color(&s.tile, lv_color_hex(color::TileEdge));
    init(s.tile_on);
    lv_style_set_bg_color(&s.tile_on, lv_color_hex(color::TileOn));
    lv_style_set_border_color(&s.tile_on, lv_color_hex(color::TileOnEdge));
    init(s.tile_pressed);
    lv_style_set_bg_color(&s.tile_pressed, lv_color_hex(color::TilePressed));
    init(s.glyph);
    fill(s.glyph, color::Glyph);
    ink(s.glyph, color::GlyphInk);
    lv_style_set_radius(&s.glyph, 8);
    lv_style_set_text_align(&s.glyph, LV_TEXT_ALIGN_CENTER);
    init(s.glyph_on);
    lv_style_set_bg_color(&s.glyph_on, lv_color_hex(color::Amber));
    ink(s.glyph_on, color::AmberInk);
    init(s.name);
    ink(s.name, color::Ink);
    init(s.state);
    ink(s.state, color::State);
    init(s.state_on);
    ink(s.state_on, color::StateOn);
    init(s.step);
    fill(s.step, color::Control);
    ink(s.step, color::ControlInk);
    lv_style_set_radius(&s.step, 6);
    lv_style_set_text_align(&s.step, LV_TEXT_ALIGN_CENTER);
    init(s.step_pressed);
    lv_style_set_bg_color(&s.step_pressed, lv_color_hex(color::ControlPressed));
    init(s.pager);
    fill(s.pager, color::Control);
    ink(s.pager, color::ControlInk);
    lv_style_set_radius(&s.pager, 8);
    lv_style_set_text_align(&s.pager, LV_TEXT_ALIGN_CENTER);
    init(s.dim);
    lv_style_set_opa(&s.dim, LV_OPA_40);

    init(s.scene);
    fill(s.scene, color::Scene);
    ink(s.scene, color::ControlInk);
    lv_style_set_radius(&s.scene, 10);
    lv_style_set_border_width(&s.scene, 1);
    lv_style_set_border_color(&s.scene, lv_color_hex(color::SceneEdge));
    lv_style_set_pad_hor(&s.scene, 16);
    init(s.scene_pressed);
    lv_style_set_bg_color(&s.scene_pressed, lv_color_hex(color::ScenePressed));
    init(s.activity);
    ink(s.activity, color::State);
    lv_style_set_text_align(&s.activity, LV_TEXT_ALIGN_RIGHT);

    init(s.card);
    fill(s.card, color::Card);
    lv_style_set_radius(&s.card, 14);
    lv_style_set_border_width(&s.card, 1);
    lv_style_set_border_color(&s.card, lv_color_hex(color::CardEdge));
    lv_style_set_pad_ver(&s.card, 12);
    lv_style_set_pad_hor(&s.card, 16);
    lv_style_set_pad_row(&s.card, 6);
    init(s.heard);
    ink(s.heard, color::State);
    init(s.message);
    ink(s.message, color::Message);
    init(s.message_attention);
    ink(s.message_attention, color::Attention);
    init(s.message_error);
    ink(s.message_error, color::MessageError);
    init(s.choice);
    fill(s.choice, color::Amber);
    ink(s.choice, color::AmberInk);
    lv_style_set_radius(&s.choice, 9);
    lv_style_set_pad_hor(&s.choice, 14);
    init(s.toast);
    fill(s.toast, color::Toast);
    ink(s.toast, 0xFFFFFF);
    lv_style_set_radius(&s.toast, 8);
    lv_style_set_pad_ver(&s.toast, 6);
    lv_style_set_pad_hor(&s.toast, 14);

    init(s.sys_title);
    ink(s.sys_title, color::Ink);
    lv_style_set_text_align(&s.sys_title, LV_TEXT_ALIGN_CENTER);
    init(s.sys_attention);
    ink(s.sys_attention, color::Attention);
    init(s.sys_error);
    ink(s.sys_error, color::Error);
    init(s.sys_detail);
    ink(s.sys_detail, color::State);
    lv_style_set_text_align(&s.sys_detail, LV_TEXT_ALIGN_CENTER);
    init(s.sys_action);
    fill(s.sys_action, color::Control);
    ink(s.sys_action, color::ControlInk);
    lv_style_set_radius(&s.sys_action, 10);
    lv_style_set_pad_ver(&s.sys_action, 10);
    lv_style_set_pad_hor(&s.sys_action, 24);
}

void HomePanelView::Build(const BuildContext& ctx) {
    if (built_) return;
    display_ = ctx.display;
    font_ = ctx.font;
    icon_font_ = ctx.icon_font != nullptr ? ctx.icon_font : ctx.font;
    BuildStyles();
    lv_style_set_text_font(&S().root, font_);
    lv_style_set_text_font(&S().mic, icon_font_);
    lv_style_set_text_font(&S().pager, icon_font_);

    root_ = NewBox(ctx.parent, &S().root);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(root_, kWidth, kHeight);
    // One handler at the root for every touchable object (they bubble), and
    // only for the input events it acts on: never draw or layout events.
    for (lv_event_code_t code : {LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(root_, OnEvent, code, this);
    }

    dash_ = NewBox(root_);
    lv_obj_set_size(dash_, kWidth, kHeight);
    BuildTopBar();
    BuildNav();
    BuildGrid();
    BuildBottomBar();
    BuildCard();
    BuildSystemPage();

    toast_ = NewLabel(root_, &S().toast);
    lv_obj_align(toast_, LV_ALIGN_TOP_MID, 0, kTopH + 6);
    Visible(toast_, false);

    built_ = true;
    ApplyFontMetrics();
    RebuildNavigation();
    RenderScene();
    timer_ = lv_timer_create(OnTimer, 1000, this);
}

void HomePanelView::BuildTopBar() {
    top_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(top_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_size(top_, kWidth, kTopH);

    lv_obj_t* left = NewBox(top_);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, 0);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 16, 0);
    home_label_ = NewLabel(left);
    area_label_ = NewLabel(left, &S().muted);

    clock_label_ = NewLabel(top_, &S().clock);
    lv_label_set_text_static(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* right = NewBox(top_);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -12, 0);
    conn_dot_ = NewBox(right, &S().dot);
    lv_obj_add_style(conn_dot_, &S().dot_on, LV_STATE_CHECKED);
    lv_obj_set_size(conn_dot_, 8, 8);
    conn_label_ = NewLabel(right, &S().conn);
    lv_obj_set_style_margin_right(conn_label_, 10, 0);
    mic_ = NewLabel(right, &S().mic);
    lv_obj_add_style(mic_, &S().mic_live, LV_STATE_CHECKED);
    lv_obj_add_style(mic_, &S().mic_busy, LV_STATE_USER_1);
    lv_obj_set_size(mic_, 30, 30);
    lv_label_set_text_static(mic_, FONT_AWESOME_MICROPHONE);
    Touchable(mic_, Tag(static_cast<uint8_t>(Role::Mic), 0));
}

void HomePanelView::BuildNav() {
    nav_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(nav_, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_pos(nav_, 0, kTopH);
    lv_obj_set_size(nav_, kNavW, kMidH);
    lv_obj_set_style_pad_ver(nav_, 10, 0);
    lv_obj_set_style_pad_hor(nav_, 8, 0);
    lv_obj_set_style_pad_row(nav_, 4, 0);
    lv_obj_set_flex_flow(nav_, LV_FLEX_FLOW_COLUMN);
    // A registry can hold up to 32 areas; the column scrolls when it must.
    lv_obj_add_flag(nav_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nav_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav_, LV_SCROLLBAR_MODE_OFF);
}

HomePanelView::NavSlot& HomePanelView::NavSlotAt(size_t index) {
    while (nav_slots_.size() <= index) {
        NavSlot slot;
        slot.item = NewLabel(nav_, &S().nav);
        lv_obj_add_style(slot.item, &S().nav_on, LV_STATE_CHECKED);
        lv_obj_set_width(slot.item, kNavW - 16);
        lv_label_set_long_mode(slot.item, LV_LABEL_LONG_MODE_DOTS);
        Touchable(slot.item, Tag(static_cast<uint8_t>(Role::Nav), nav_slots_.size()));
        slot.count = NewLabel(slot.item, &S().count);
        lv_obj_align(slot.count, LV_ALIGN_RIGHT_MID, 24, 0);
        nav_slots_.push_back(slot);
    }
    return nav_slots_[index];
}

void HomePanelView::BuildGrid() {
    grid_ = NewBox(dash_);
    lv_obj_add_style(grid_, &S().dim, LV_STATE_DISABLED);
    lv_obj_set_pos(grid_, kNavW, kTopH);
    lv_obj_set_size(grid_, kGridW, kMidH);
    for (size_t i = 0; i < kTileSlots; ++i) {
        TileSlot& t = tiles_[i];
        const int column = static_cast<int>(i) % kCols;
        const int row = static_cast<int>(i) / kCols;
        t.box = NewBox(grid_, &S().tile);
        lv_obj_add_style(t.box, &S().tile_on, LV_STATE_CHECKED);
        lv_obj_add_style(t.box, &S().tile_pressed, LV_STATE_PRESSED);
        lv_obj_set_pos(t.box, kGridPad + column * (kTileW + kGap), kGridPad + row * (kTileH + kGap));
        lv_obj_set_size(t.box, kTileW, kTileH);
        Touchable(t.box, Tag(static_cast<uint8_t>(Role::Tile), i));
        t.glyph = NewLabel(t.box, &S().glyph);
        lv_obj_add_style(t.glyph, &S().glyph_on, LV_STATE_CHECKED);
        t.name = NewLabel(t.box, &S().name);
        lv_label_set_long_mode(t.name, LV_LABEL_LONG_MODE_DOTS);
        t.state = NewLabel(t.box, &S().state);
        lv_obj_add_style(t.state, &S().state_on, LV_STATE_CHECKED);
        lv_label_set_long_mode(t.state, LV_LABEL_LONG_MODE_DOTS);
        t.minus = NewLabel(t.box, &S().step);
        lv_obj_add_style(t.minus, &S().step_pressed, LV_STATE_PRESSED);
        lv_label_set_text_static(t.minus, "-");
        Touchable(t.minus, Tag(static_cast<uint8_t>(Role::Minus), i));
        t.plus = NewLabel(t.box, &S().step);
        lv_obj_add_style(t.plus, &S().step_pressed, LV_STATE_PRESSED);
        lv_label_set_text_static(t.plus, "+");
        Touchable(t.plus, Tag(static_cast<uint8_t>(Role::Plus), i));
        Visible(t.box, false);
    }

    empty_label_ = NewLabel(grid_, &S().muted);
    lv_label_set_text_static(empty_label_, "还没有家居设备 · 请在手机「智能家居」里添加");
    lv_obj_align(empty_label_, LV_ALIGN_CENTER, 0, -20);

    page_prev_ = NewLabel(grid_, &S().pager);
    lv_label_set_text_static(page_prev_, FONT_AWESOME_ANGLE_LEFT);
    lv_obj_set_size(page_prev_, 60, kPagerH);
    lv_obj_set_pos(page_prev_, kGridW / 2 - 110, kPagerY);
    Touchable(page_prev_, Tag(static_cast<uint8_t>(Role::PagePrev), 0));
    page_label_ = NewLabel(grid_, &S().muted);
    lv_obj_set_style_text_align(page_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(page_label_, 80);
    page_next_ = NewLabel(grid_, &S().pager);
    lv_label_set_text_static(page_next_, FONT_AWESOME_ANGLE_RIGHT);
    lv_obj_set_size(page_next_, 60, kPagerH);
    lv_obj_set_pos(page_next_, kGridW / 2 + 50, kPagerY);
    Touchable(page_next_, Tag(static_cast<uint8_t>(Role::PageNext), 0));
}

void HomePanelView::BuildBottomBar() {
    bottom_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(bottom_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_pos(bottom_, 0, kHeight - kBottomH);
    lv_obj_set_size(bottom_, kWidth, kBottomH);
    lv_obj_set_style_pad_hor(bottom_, 12, 0);
    lv_obj_set_style_pad_column(bottom_, 8, 0);
    lv_obj_set_flex_flow(bottom_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < kSceneSlots; ++i) {
        scenes_[i] = NewLabel(bottom_, &S().scene);
        lv_obj_add_style(scenes_[i], &S().scene_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(scenes_[i], &S().dim, LV_STATE_DISABLED);
        Touchable(scenes_[i], Tag(static_cast<uint8_t>(Role::Scene), i));
        Visible(scenes_[i], false);
    }
    activity_label_ = NewLabel(bottom_, &S().activity);
    lv_label_set_long_mode(activity_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(activity_label_, 1);
}

void HomePanelView::BuildCard() {
    card_ = NewBox(root_, &S().card);
    lv_obj_set_size(card_, kWidth - 2 * kCardMargin, LV_SIZE_CONTENT);
    lv_obj_align(card_, LV_ALIGN_BOTTOM_MID, 0, -(kBottomH + 14));
    lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_COLUMN);
    // The card swallows taps (they must not reach the tile underneath) and a
    // tap on its body dismisses it early; the choices stay tappable.
    Touchable(card_, Tag(static_cast<uint8_t>(Role::Card), 0));
    card_heard_ = NewLabel(card_, &S().heard);
    lv_obj_set_width(card_heard_, lv_pct(100));
    lv_label_set_long_mode(card_heard_, LV_LABEL_LONG_MODE_DOTS);
    card_message_ = NewLabel(card_, &S().message);
    lv_obj_add_style(card_message_, &S().message_attention, LV_STATE_USER_2);
    lv_obj_add_style(card_message_, &S().message_error, LV_STATE_USER_1);
    lv_obj_set_width(card_message_, lv_pct(100));
    lv_label_set_long_mode(card_message_, LV_LABEL_LONG_MODE_WRAP);
    card_choices_ = NewBox(card_);
    lv_obj_set_size(card_choices_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card_choices_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(card_choices_, 8, 0);
    lv_obj_set_style_pad_row(card_choices_, 8, 0);
    for (size_t i = 0; i < kCandidateSlots; ++i) {
        candidates_[i] = NewLabel(card_choices_, &S().choice);
        Touchable(candidates_[i], Tag(static_cast<uint8_t>(Role::Candidate), i));
        Visible(candidates_[i], false);
    }
    Visible(card_, false);
}

void HomePanelView::BuildSystemPage() {
    sys_ = NewBox(root_);
    lv_obj_set_size(sys_, kWidth, kHeight);
    sys_title_ = NewLabel(sys_, &S().sys_title);
    lv_obj_add_style(sys_title_, &S().sys_attention, LV_STATE_USER_1);
    lv_obj_add_style(sys_title_, &S().sys_error, LV_STATE_USER_2);
    lv_obj_set_width(sys_title_, kWidth - 120);
    lv_obj_align(sys_title_, LV_ALIGN_CENTER, 0, -70);
    sys_detail_ = NewLabel(sys_, &S().sys_detail);
    lv_obj_set_width(sys_detail_, kWidth - 160);
    lv_label_set_long_mode(sys_detail_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(sys_detail_, LV_ALIGN_CENTER, 0, -10);
    sys_action_ = NewLabel(sys_, &S().sys_action);
    lv_obj_align(sys_action_, LV_ALIGN_CENTER, 0, 80);
    Touchable(sys_action_, Tag(static_cast<uint8_t>(Role::Primary), 0));
    sys_setup_ = NewLabel(sys_, &S().sys_action);
    lv_label_set_text_static(sys_setup_, "Setup");
    lv_obj_align(sys_setup_, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    Touchable(sys_setup_, Tag(static_cast<uint8_t>(Role::Setup), 0));
    Visible(sys_action_, false);
    Visible(sys_setup_, false);
    Visible(sys_, false);
}

// Everything that depends on the text font's metrics. Labels are not
// containers: fixed-height "buttons" center their text with padding.
void HomePanelView::ApplyFontMetrics() {
    if (!built_) return;
    Styles& s = S();
    const int line = lv_font_get_line_height(font_);
    const int icon_line = lv_font_get_line_height(icon_font_);
    lv_style_set_pad_ver(&s.nav, PadFor(36, line));
    lv_style_set_pad_ver(&s.scene, PadFor(40, line));
    lv_style_set_pad_ver(&s.choice, PadFor(34, line));
    lv_style_set_pad_top(&s.mic, PadFor(30, icon_line));
    lv_style_set_pad_top(&s.pager, PadFor(kPagerH, icon_line));
    const int glyph = std::max(28, line + 2);
    lv_style_set_pad_top(&s.glyph, PadFor(glyph, line));
    const int step_h = std::max(26, line);
    lv_style_set_pad_top(&s.step, PadFor(step_h, line));
    lv_obj_report_style_change(nullptr);

    const int name_x = kTilePad + glyph + 8;
    for (TileSlot& t : tiles_) {
        lv_obj_set_pos(t.glyph, kTilePad, kTilePad);
        lv_obj_set_size(t.glyph, glyph, glyph);
        lv_obj_set_pos(t.name, name_x, kTilePad + (glyph - line) / 2);
        lv_obj_set_size(t.name, kTileW - name_x - kTilePad, line);
        lv_obj_set_pos(t.state, kTilePad, kTilePad + glyph + 4);
        lv_obj_set_size(t.state, kTileW - 2 * kTilePad, line);
        const int step_y = kTileH - kTilePad - step_h;
        lv_obj_set_pos(t.plus, kTileW - kTilePad - kStepW, step_y);
        lv_obj_set_size(t.plus, kStepW, step_h);
        lv_obj_set_pos(t.minus, kTileW - kTilePad - 2 * kStepW - 6, step_y);
        lv_obj_set_size(t.minus, kStepW, step_h);
    }
    lv_obj_set_height(activity_label_, line);
    lv_obj_set_pos(page_label_, kGridW / 2 - 40, kPagerY + PadFor(kPagerH, line));
    rendered_generation_ = UINT32_MAX;  // widths changed; re-measure state texts
}

void HomePanelView::SetFont(const lv_font_t* font) {
    if (font == nullptr || font == font_ || !built_) return;
    font_ = font;
    lv_style_set_text_font(&S().root, font_);
    ApplyFontMetrics();
    RenderScene();
}

// --- Surface -----------------------------------------------------------------

smarthome::ApplyOutcome HomePanelView::Apply(smarthome::Message&& message) {
    ViewLock lock(display_);
    auto outcome = smarthome::ApplyOutcome::Applied;
    switch (message.kind) {
    case smarthome::MessageKind::Snapshot:
        store_.ApplySnapshot(std::move(message.snapshot));
        ESP_LOGI(kTag, "[smarthome] snapshot revision=%llu devices=%u areas=%u scenes=%u",
                 static_cast<unsigned long long>(store_.revision()),
                 static_cast<unsigned>(store_.home().devices.size()),
                 static_cast<unsigned>(store_.home().areas.size()),
                 static_cast<unsigned>(store_.home().scenes.size()));
        LogMissingGlyphs();
        if (built_) RebuildNavigation();
        break;
    case smarthome::MessageKind::Delta:
        outcome = store_.ApplyDelta(message.delta, NowUtcMs());
        if (outcome == smarthome::ApplyOutcome::NeedSync) {
            ESP_LOGW(kTag, "[smarthome] delta revision=%llu seq=%llu not applicable to %llu/%llu",
                     static_cast<unsigned long long>(message.delta.revision),
                     static_cast<unsigned long long>(message.delta.seq),
                     static_cast<unsigned long long>(store_.revision()),
                     static_cast<unsigned long long>(store_.seq()));
            RequestSync();
        } else if (outcome == smarthome::ApplyOutcome::Ignored) {
            ESP_LOGD(kTag, "[smarthome] delta seq=%llu repeats held seq=%llu",
                     static_cast<unsigned long long>(message.delta.seq),
                     static_cast<unsigned long long>(store_.seq()));
        }
        break;
    case smarthome::MessageKind::Result:
        ShowResult(std::move(message.result));
        break;
    }
    if (built_) RenderScene();
    return outcome;
}

void HomePanelView::SetLinkUp(bool up) {
    ViewLock lock(display_);
    store_.SetLinkUp(up);
    if (up) RequestSync();
    if (built_) RenderScene();
}

void HomePanelView::SetWallClock(int64_t utc_ms) {
    ViewLock lock(display_);
    clock_utc_ms_ = utc_ms > 0 ? utc_ms : 0;
    clock_mono_ms_ = MonoMs();
    if (built_) Text(clock_label_, LocalClock());
}

void HomePanelView::SetSink(smarthome::PanelSink* sink) {
    ViewLock lock(display_);
    sink_ = sink != nullptr ? sink : &logging_sink_;
}

void HomePanelView::ShowNotification(const char* text, int duration_ms) {
    if (!built_) return;
    DisplayLockGuard lock(display_);
    Text(toast_, text);
    Visible(toast_, text != nullptr && *text != '\0');
    toast_until_ms_ = MonoMs() + std::clamp(duration_ms, 500, 15000);
}

int64_t HomePanelView::NowUtcMs() const {
    return clock_utc_ms_ > 0 ? clock_utc_ms_ + (MonoMs() - clock_mono_ms_) : 0;
}

// Local time needs the home's offset, which only a snapshot carries; showing
// UTC as if it were local would be worse than showing nothing.
std::string HomePanelView::LocalClock() const {
    if (!store_.has_snapshot()) return smarthome::FormatClock(0, 0);
    return smarthome::FormatClock(NowUtcMs(), store_.home().utc_offset_minutes);
}

void HomePanelView::RequestSync() {
    if (!store_.ClaimSyncRequest(MonoMs())) return;
    sink_->SendRequest(smarthome::BuildSyncRequest(store_.has_snapshot(), store_.revision(), store_.seq()));
}

std::string HomePanelView::NextRequestId() {
    if (!ids_seeded_) {
        // The first touch happens long after Wi-Fi is up, so the hardware RNG
        // is fed by the radio; the id must not repeat across reboots.
        request_ids_ = smarthome::RequestIdSource(esp_random());
        ids_seeded_ = true;
    }
    return request_ids_.Next();
}

void HomePanelView::Send(const smarthome::Command& command) {
    const std::string request = smarthome::BuildExecuteRequest(NextRequestId(), {command});
    if (request.empty()) {
        ESP_LOGW(kTag, "[smarthome] command for %s cannot be expressed on the wire", command.device_id.c_str());
        return;
    }
    sink_->SendRequest(request);
}

void HomePanelView::SendScene(const std::string& scene_id) {
    const std::string request = smarthome::BuildSceneRequest(NextRequestId(), scene_id);
    if (request.empty()) {
        ESP_LOGW(kTag, "[smarthome] scene %s cannot be expressed on the wire", scene_id.c_str());
        return;
    }
    sink_->SendRequest(request);
}

// Device names are free text typed on the phone, so a name can hold a
// character the panel font lacks; say so instead of silently dropping it.
void HomePanelView::LogMissingGlyphs() const {
    if (font_ == nullptr) return;
    size_t missing = 0;
    uint32_t example = 0;
    const std::string* where = nullptr;
    const auto scan = [&](const std::string& text) {
        const char* p = text.c_str();
        for (uint32_t cp = NextCodePoint(p); cp != 0; cp = NextCodePoint(p)) {
            lv_font_glyph_dsc_t dsc;
            if (cp < 0x20 || lv_font_get_glyph_dsc(font_, &dsc, cp, 0)) continue;
            if (missing++ == 0) {
                example = cp;
                where = &text;
            }
        }
    };
    const auto& home = store_.home();
    scan(home.home_name);
    for (const auto& area : home.areas) scan(area.name);
    for (const auto& device : home.devices) scan(device.name);
    for (const auto& scene : home.scenes) scan(scene.name);
    if (missing > 0) {
        ESP_LOGW(kTag, "[smarthome] %u character(s) not in the panel font, first U+%04X in \"%s\"",
                 static_cast<unsigned>(missing), static_cast<unsigned>(example), where->c_str());
    }
}

// --- Rendering ---------------------------------------------------------------

bool HomePanelView::DashboardVisible() const {
    switch (scene_) {
    case UiScene::Ready:
    case UiScene::OpeningConversation:
    case UiScene::Conversation:
    case UiScene::Ended:
        return true;
    case UiScene::Reconnecting:
        // Losing the Host is exactly when the last state is worth showing,
        // dimmed; without one there is nothing to show but the recovery page.
        return store_.has_snapshot();
    default:
        return false;
    }
}

bool HomePanelView::Stale() const {
    return store_.stale() || (scene_ == UiScene::Reconnecting && store_.has_snapshot());
}

void HomePanelView::Render(const EidolonUiModel& model) {
    if (!built_) return;
    DisplayLockGuard lock(display_);
    scene_ = model.scene;
    turn_ = model.turn;
    mode_ = model.interaction_mode;
    mic_muted_ = model.show_mute_icon;
    const bool touch = model.primary_presentation == UiActionPresentation::TouchControl && model.primary_enabled;
    mic_intent_ = touch ? model.primary_intent : UiIntent::None;
    subtitle_ = model.subtitle != nullptr ? model.subtitle : "";
    subtitle_from_user_ = model.subtitle_role != nullptr && std::strcmp(model.subtitle_role, "user") == 0;

    // A spoken turn is visible from the first moment: no speaker means the
    // card is the only feedback there is.
    const bool conversation = scene_ == UiScene::Conversation;
    if (conversation && IsTalking(turn_)) {
        card_mode_ = CardMode::Listening;
    } else if (conversation && IsBusy(turn_) && card_mode_ != CardMode::Result) {
        card_mode_ = CardMode::Processing;
    } else if (card_mode_ == CardMode::Listening || card_mode_ == CardMode::Processing) {
        card_mode_ = CardMode::Hidden;
    }

    // System page text is copied now: model strings belong to the presenter.
    Text(sys_title_, model.status_text);
    Text(sys_detail_, model.detail_text);
    SetState(sys_title_, LV_STATE_USER_1, model.severity == UiSeverity::Attention);
    SetState(sys_title_, LV_STATE_USER_2, model.severity == UiSeverity::Error);
    sys_intent_ = touch && (model.primary_intent == UiIntent::OpenConversation ||
                            model.primary_intent == UiIntent::ToggleMicrophone)
        ? model.primary_intent : UiIntent::None;
    if (sys_intent_ != UiIntent::None) {
        Text(sys_action_, model.primary_label);
    } else if (model.primary_presentation == UiActionPresentation::InputHint) {
        Text(sys_action_, model.input_hint);
    } else {
        Text(sys_action_, "");
    }
    Visible(sys_action_, lv_label_get_text(sys_action_)[0] != '\0');
    Visible(sys_setup_, model.show_setup_action);
    RenderScene();
}

void HomePanelView::RenderScene() {
    const bool dashboard = DashboardVisible();
    Visible(dash_, dashboard);
    Visible(sys_, !dashboard);
    if (dashboard) {
        RenderDashboard();
    } else {
        Visible(card_, false);
    }
}

void HomePanelView::RenderDashboard() {
    RenderTopBar();
    RenderMic();
    if (rendered_generation_ != store_.generation()) {
        RenderNav();
        RenderGrid();
        RenderBottomBar();
        rendered_generation_ = store_.generation();
    }
    const bool stale = Stale();
    SetState(grid_, LV_STATE_DISABLED, stale);
    for (lv_obj_t* scene : scenes_) SetState(scene, LV_STATE_DISABLED, stale);
    RenderCard();
}

void HomePanelView::RenderTopBar() {
    Text(home_label_, store_.has_snapshot() ? store_.home().home_name : std::string("我的家"));
    const char* area = "全部";
    for (const auto& entry : nav_entries_) {
        if (entry.area_id == area_) area = entry.name.c_str();
    }
    Text(area_label_, area);

    const char* link = "已连接";
    bool online = false;
    if (scene_ == UiScene::Reconnecting || (store_.has_snapshot() && !store_.link_up())) {
        link = "离线 · 上次状态";
    } else if (!store_.has_snapshot()) {
        link = store_.link_up() ? "同步中" : "未同步";
    } else if (store_.awaiting_snapshot()) {
        link = "同步中";
    } else {
        online = true;
    }
    Text(conn_label_, link);
    SetState(conn_dot_, LV_STATE_CHECKED, online);
    Text(clock_label_, LocalClock());
}

void HomePanelView::RenderMic() {
    const bool conversation = scene_ == UiScene::Conversation;
    const bool live = conversation && !mic_muted_ &&
        (IsTalking(turn_) || (turn_ == TurnPhase::Idle && !IsPushToTalk(mode_)));
    SetState(mic_, LV_STATE_CHECKED, live);
    SetState(mic_, LV_STATE_USER_1, conversation && IsBusy(turn_));
    TextStatic(mic_, conversation && mic_muted_ ? FONT_AWESOME_MICROPHONE_SLASH : FONT_AWESOME_MICROPHONE);
}

void HomePanelView::RebuildNavigation() {
    nav_entries_ = smarthome::BuildNav(store_.home());
    const bool kept = std::any_of(nav_entries_.begin(), nav_entries_.end(),
                                  [&](const smarthome::NavEntry& e) { return e.area_id == area_; });
    if (!kept) {
        area_ = smarthome::DefaultArea(store_.home());
        page_ = 0;
    }
    for (size_t i = 0; i < nav_entries_.size(); ++i) NavSlotAt(i);
    rendered_generation_ = UINT32_MAX;
}

void HomePanelView::RenderNav() {
    char count[8];
    for (size_t i = 0; i < nav_slots_.size(); ++i) {
        NavSlot& slot = nav_slots_[i];
        const bool used = i < nav_entries_.size();
        Visible(slot.item, used);
        if (!used) continue;
        Text(slot.item, nav_entries_[i].name);
        std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(nav_entries_[i].count));
        Text(slot.count, count);
        SetState(slot.item, LV_STATE_CHECKED, nav_entries_[i].area_id == area_);
    }
}

void HomePanelView::RenderGrid() {
    const auto& devices = store_.home().devices;
    size_t shown = 0;
    for (const auto& device : devices) shown += smarthome::InArea(device, area_);
    const size_t pages = std::max<size_t>(1, (shown + kTileSlots - 1) / kTileSlots);
    page_ = std::min(page_, pages - 1);

    // Bind the fixed tile pool to this page of the selected area.
    const size_t first = page_ * kTileSlots;
    size_t ordinal = 0;
    size_t slot = 0;
    const int text_width = kTileW - 2 * kTilePad;
    for (size_t i = 0; i < devices.size() && slot < kTileSlots; ++i) {
        const smarthome::Device& device = devices[i];
        if (!smarthome::InArea(device, area_) || ordinal++ < first) continue;
        TileSlot& t = tiles_[slot++];
        t.device = static_cast<int>(i);
        TextStatic(t.glyph, smarthome::DeviceGlyph(device));
        Text(t.name, device.name);
        std::string state = smarthome::DeviceStateText(device);
        lv_point_t size;
        lv_text_get_size(&size, state.c_str(), font_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > text_width) state = smarthome::DeviceStateText(device, true);
        Text(t.state, state);
        const bool active = smarthome::IsActive(device);
        SetState(t.box, LV_STATE_CHECKED, active);
        SetState(t.glyph, LV_STATE_CHECKED, active);
        SetState(t.state, LV_STATE_CHECKED, active);
        const bool steps = smarthome::OffersSteps(device);
        Visible(t.minus, steps);
        Visible(t.plus, steps);
        Visible(t.box, true);
    }
    for (; slot < kTileSlots; ++slot) {
        tiles_[slot].device = -1;
        Visible(tiles_[slot].box, false);
    }

    Visible(empty_label_, devices.empty());
    const bool paged = pages > 1;
    Visible(page_prev_, paged);
    Visible(page_next_, paged);
    Visible(page_label_, paged);
    if (paged) {
        char label[16];
        std::snprintf(label, sizeof(label), "%u / %u", static_cast<unsigned>(page_ + 1),
                      static_cast<unsigned>(pages));
        Text(page_label_, label);
    }
}

void HomePanelView::RenderBottomBar() {
    const auto& scenes = store_.home().scenes;
    for (size_t i = 0; i < kSceneSlots; ++i) {
        const bool used = i < scenes.size();
        Visible(scenes_[i], used);
        if (used) Text(scenes_[i], scenes[i].name);
    }
    Text(activity_label_, smarthome::FormatActivity(store_.activity(), store_.home().utc_offset_minutes));
}

void HomePanelView::ShowResult(smarthome::VoiceResult&& result) {
    result_ = std::move(result);
    card_mode_ = CardMode::Result;
    const bool choices = result_.outcome == smarthome::VoiceOutcome::Ambiguous && result_.has_command;
    card_until_ms_ = MonoMs() + (choices ? kChoiceResultMs : kResultMs);
}

void HomePanelView::HideCard() {
    card_mode_ = CardMode::Hidden;
    RenderCard();
}

void HomePanelView::RenderCard() {
    const bool visible = card_mode_ != CardMode::Hidden && DashboardVisible();
    Visible(card_, visible);
    if (!visible) return;
    const bool result = card_mode_ == CardMode::Result;
    if (result) {
        Text(card_heard_, result_.utterance.empty() ? std::string() : "「" + result_.utterance + "」");
        Text(card_message_, result_.message);
    } else {
        Text(card_heard_, subtitle_from_user_ && !subtitle_.empty() ? "「" + subtitle_ + "」" : std::string());
        TextStatic(card_message_, card_mode_ == CardMode::Listening ? "聆听中…" : "处理中…");
    }
    Visible(card_heard_, lv_label_get_text(card_heard_)[0] != '\0');
    const auto tone = result ? smarthome::OutcomeTone(result_.outcome) : smarthome::ResultTone::Normal;
    SetState(card_message_, LV_STATE_USER_1, tone == smarthome::ResultTone::Error);
    SetState(card_message_, LV_STATE_USER_2, tone == smarthome::ResultTone::Attention);
    const bool choices = result && result_.outcome == smarthome::VoiceOutcome::Ambiguous && result_.has_command;
    size_t shown = 0;
    for (size_t i = 0; i < kCandidateSlots; ++i) {
        const bool used = choices && i < result_.candidates.size();
        Visible(candidates_[i], used);
        if (used) {
            Text(candidates_[i], result_.candidates[i].name);
            ++shown;
        }
    }
    Visible(card_choices_, shown > 0);
}

void HomePanelView::OnTimer(lv_timer_t* timer) {
    static_cast<HomePanelView*>(lv_timer_get_user_data(timer))->Tick();
}

// LVGL task, lock held. Cheap by construction: string compares, no layout.
void HomePanelView::Tick() {
    const int64_t now = MonoMs();
    Text(clock_label_, LocalClock());
    if (card_mode_ == CardMode::Result && now >= card_until_ms_) HideCard();
    if (toast_until_ms_ != 0 && now >= toast_until_ms_) {
        toast_until_ms_ = 0;
        Visible(toast_, false);
    }
}

// --- Input -------------------------------------------------------------------

void HomePanelView::OnEvent(lv_event_t* event) {
    static_cast<HomePanelView*>(lv_event_get_user_data(event))->HandleEvent(event);
}

// LVGL task, lock held. Commands go to the sink; nothing here changes a tile.
void HomePanelView::HandleEvent(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (target == nullptr) return;
    const void* tag = lv_obj_get_user_data(target);
    const auto role = static_cast<Role>(TagRole(tag));
    const size_t index = TagIndex(tag);
    if (role == Role::Mic) {
        HandleMic(code);
        return;
    }
    if (code != LV_EVENT_CLICKED) return;
    switch (role) {
    case Role::Tile:
    case Role::Minus:
    case Role::Plus: {
        if (Stale() || index >= kTileSlots) return;
        const int device = tiles_[index].device;
        if (device < 0 || static_cast<size_t>(device) >= store_.home().devices.size()) return;
        const auto action = role == Role::Tile ? smarthome::TileAction::Tap
            : role == Role::Minus ? smarthome::TileAction::Minus : smarthome::TileAction::Plus;
        smarthome::Command command;
        if (smarthome::TileCommand(store_.home().devices[device], action, command)) Send(command);
        return;
    }
    case Role::Nav:
        if (index < nav_entries_.size() && nav_entries_[index].area_id != area_) {
            area_ = nav_entries_[index].area_id;
            page_ = 0;
            rendered_generation_ = UINT32_MAX;
            RenderDashboard();
        }
        return;
    case Role::PagePrev:
    case Role::PageNext:
        if (role == Role::PagePrev && page_ > 0) --page_;
        else if (role == Role::PageNext) ++page_;  // clamped by RenderGrid
        rendered_generation_ = UINT32_MAX;
        RenderDashboard();
        return;
    case Role::Scene:
        // The host expands the scene; the panel only names it.
        if (!Stale() && index < store_.home().scenes.size()) SendScene(store_.home().scenes[index].id);
        return;
    case Role::Candidate:
        if (Stale() || card_mode_ != CardMode::Result || !result_.has_command ||
            index >= result_.candidates.size())
            return;
        {
            smarthome::Command command = result_.command;
            command.device_id = result_.candidates[index].device_id;
            Send(command);
        }
        HideCard();
        return;
    case Role::Card:
        HideCard();
        return;
    case Role::Primary:
        if (sys_intent_ != UiIntent::None) DispatchEidolonUiIntent(sys_intent_);
        return;
    case Role::Setup:
        DispatchEidolonUiIntent(UiIntent::OpenSetup);
        return;
    default:
        return;
    }
}

void HomePanelView::HandleMic(lv_event_code_t code) {
    if (code == LV_EVENT_PRESSED && mic_intent_ == UiIntent::BeginTalk) {
        talk_active_ = true;
        DispatchEidolonUiIntent(UiIntent::BeginTalk);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && talk_active_) {
        talk_active_ = false;
        DispatchEidolonUiIntent(UiIntent::CommitTalk);
    } else if (code == LV_EVENT_CLICKED &&
               (mic_intent_ == UiIntent::OpenConversation || mic_intent_ == UiIntent::ToggleMicrophone)) {
        DispatchEidolonUiIntent(mic_intent_);
    }
}

}  // namespace eidolon
