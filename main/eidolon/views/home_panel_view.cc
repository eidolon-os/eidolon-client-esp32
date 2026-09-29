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
#include "eidolon/views/home_panel_copy.h"
#include "eidolon/views/home_panel_icons.h"

LV_FONT_DECLARE(font_home_icons_20);
LV_FONT_DECLARE(font_home_text_20);
LV_FONT_DECLARE(font_home_icons_24);
LV_FONT_DECLARE(font_home_icons_48);
LV_FONT_DECLARE(font_home_clock_34);

namespace eidolon {

namespace {

constexpr const char* kTag = "HomePanel";

// 800x480: 48 px top bar, 120 px area navigation, 4 x 3 tiles and a pager
// band, 68 px bottom bar: one status line and the microphone.
constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr int kTopH = 48;
constexpr int kBottomH = 68;
constexpr int kNavW = 120;
constexpr int kNavItemH = 40;
constexpr int kNavPadLeft = 14;
constexpr int kNavCountPad = 28;  // right padding, where the device count sits
constexpr int kMidH = kHeight - kTopH - kBottomH;
constexpr int kGridW = kWidth - kNavW;
constexpr int kGridPad = 10;
constexpr int kGap = 8;
constexpr int kCols = 4;
constexpr int kRows = 3;
constexpr int kTileW = (kGridW - 2 * kGridPad - (kCols - 1) * kGap) / kCols;
constexpr int kTileH = 100;
constexpr int kTilePad = 10;
constexpr int kDisc = 36;
constexpr int kStepW = 40;
constexpr int kStepH = 34;
constexpr int kPagerY = kGridPad + kRows * kTileH + (kRows - 1) * kGap + 4;
constexpr int kPagerH = 30;
constexpr int kPagerW = 64;
constexpr int kMic = 52;
constexpr int kCardX = kNavW + kGridPad;
constexpr int kStatusDisc = 36;
constexpr int kStatusTextMaxW = 460;  // the rest of the line is the detail's
constexpr int kHeroDisc = 96;
constexpr int kResultMs = 6000;
constexpr int kChoiceResultMs = 12000;

// Eidolon brand colors (eidolon_client_mobile EidolonColors) for a dark,
// always-on panel. Neighbouring surfaces stay at least two steps apart per
// channel after RGB565 quantization, so a press or a selection is visible on
// the glass and not only in a design tool. Warm amber means one thing only:
// a device is on.
namespace color {
constexpr uint32_t Canvas = 0x070B14;
constexpr uint32_t Surface = 0x111829;     // tile, off
constexpr uint32_t Raised = 0x1B2438;      // controls, selected area
constexpr uint32_t Pressed = 0x2A3654;
constexpr uint32_t Hair = 0x1C2438;
constexpr uint32_t CardBg = 0x1F2A45;
constexpr uint32_t CardEdge = 0x34446B;
constexpr uint32_t Ink = 0xE9EEFC;
constexpr uint32_t InkDim = 0x94A1C6;
constexpr uint32_t InkFaint = 0x5E688C;
constexpr uint32_t Cyan = 0x3AD9F0;        // voice and selection
constexpr uint32_t CyanDeep = 0x0F3A4A;
constexpr uint32_t OnCyan = 0x05101C;
constexpr uint32_t Indigo = 0x6E7BF2;      // understanding a command
constexpr uint32_t Ok = 0x34D399;
constexpr uint32_t Warn = 0xFBBF24;
constexpr uint32_t Bad = 0xFB7185;
constexpr uint32_t CyanTint = 0x0E2E3C;
constexpr uint32_t OkTint = 0x0D3326;
constexpr uint32_t WarnTint = 0x3A2E0C;
constexpr uint32_t BadTint = 0x3F1624;
// A device that is on: warm paper tile, amber disc.
constexpr uint32_t TileOn = 0xF4EFE6;
constexpr uint32_t TileOnPressed = 0xDCD4C6;
constexpr uint32_t TileOnInk = 0x141A2A;
constexpr uint32_t TileOnState = 0x80581A;
constexpr uint32_t StepOn = 0xE3DBCD;
constexpr uint32_t StepOnPressed = 0xC9BFAE;
constexpr uint32_t Amber = 0xFFB547;
constexpr uint32_t OnAmber = 0x3A2300;
// Last known state while the Host is away: dark again, a trace of amber left
// on what was on, every word still readable.
constexpr uint32_t StaleOnEdge = 0x4A3B1F;
constexpr uint32_t StaleOnInk = 0xC9964A;
}  // namespace color

// Shared styles: one lv_style_t per look instead of local style arrays on
// every object, since small LVGL allocations land in internal SRAM here.
struct Styles {
    lv_style_t root, bar, home, clock, net, dot, pill, pill_bad, toast;
    lv_style_t pressed, dim_text, dim_center;
    lv_style_t mic, mic_live, mic_busy, mic_opening;
    lv_style_t nav, nav_on, count;
    lv_style_t tile, tile_on, tile_on_pressed, tile_offline, glyph, glyph_on, glyph_off;
    lv_style_t state, state_on, state_off, step, step_on, step_on_pressed;
    lv_style_t pager, empty_icon;
    lv_style_t tile_stale, glyph_stale, glyph_on_stale, state_on_stale;
    lv_style_t status_text, status_voice, status_detail;
    lv_style_t card, disc, disc_busy, disc_ok, disc_warn, disc_bad, disc_muted;
    lv_style_t choice, choice_pressed;
    lv_style_t hero, hero_attention, hero_error, sys_title, bar_bg, bar_ind, sys_action, sys_ghost;
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
    const auto bg = [](lv_style_t& style, uint32_t value) { lv_style_set_bg_color(&style, lv_color_hex(value)); };
    const auto ink = [](lv_style_t& style, uint32_t text) { lv_style_set_text_color(&style, lv_color_hex(text)); };
    const auto round = [](lv_style_t& style) { lv_style_set_radius(&style, LV_RADIUS_CIRCLE); };
    const auto center = [](lv_style_t& style) { lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER); };

    init(s.root);
    fill(s.root, color::Canvas);
    ink(s.root, color::Ink);
    init(s.bar);
    lv_style_set_border_color(&s.bar, lv_color_hex(color::Hair));
    lv_style_set_border_width(&s.bar, 1);

    // Shared looks: a press, and secondary text.
    init(s.pressed);
    bg(s.pressed, color::Pressed);
    init(s.dim_text);
    ink(s.dim_text, color::InkDim);
    init(s.dim_center);
    ink(s.dim_center, color::InkDim);
    center(s.dim_center);

    // Top bar.
    init(s.clock);
    ink(s.clock, color::Ink);
    lv_style_set_text_font(&s.clock, &font_home_clock_34);
    init(s.net);
    ink(s.net, color::InkDim);
    init(s.dot);
    round(s.dot);
    fill(s.dot, color::Ok);
    init(s.pill);
    round(s.pill);
    fill(s.pill, color::WarnTint);
    ink(s.pill, color::Warn);
    lv_style_set_pad_hor(&s.pill, 12);
    init(s.pill_bad);
    bg(s.pill_bad, color::BadTint);
    ink(s.pill_bad, color::Bad);
    init(s.toast);
    round(s.toast);
    fill(s.toast, color::Pressed);
    ink(s.toast, color::Ink);
    lv_style_set_pad_hor(&s.toast, 18);

    // Microphone: the panel's only voice feedback besides the card.
    init(s.mic);
    round(s.mic);
    fill(s.mic, color::Raised);
    ink(s.mic, color::Cyan);
    center(s.mic);
    lv_style_set_text_font(&s.mic, &font_home_icons_24);
    init(s.mic_live);
    bg(s.mic_live, color::Cyan);
    ink(s.mic_live, color::OnCyan);
    init(s.mic_busy);
    bg(s.mic_busy, color::Indigo);
    ink(s.mic_busy, 0xFFFFFF);
    init(s.mic_opening);
    bg(s.mic_opening, color::CyanDeep);
    ink(s.mic_opening, color::Cyan);

    // Area navigation.
    init(s.nav);
    lv_style_set_radius(&s.nav, 10);
    lv_style_set_pad_left(&s.nav, kNavPadLeft);
    lv_style_set_pad_right(&s.nav, kNavCountPad);
    ink(s.nav, color::InkDim);
    init(s.nav_on);
    fill(s.nav_on, color::Raised);
    ink(s.nav_on, color::Ink);
    // The accent edge counts as content inset; take it out of the padding so
    // the selected name does not shift.
    lv_style_set_border_side(&s.nav_on, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s.nav_on, 3);
    lv_style_set_border_color(&s.nav_on, lv_color_hex(color::Cyan));
    lv_style_set_pad_left(&s.nav_on, kNavPadLeft - 3);
    init(s.count);
    ink(s.count, color::InkFaint);
    lv_style_set_text_align(&s.count, LV_TEXT_ALIGN_RIGHT);

    // Tiles. Off is a dark card; on is a warm paper card with an amber disc,
    // the difference meant to read from across the room. The tile's text
    // color is its name's: the name label inherits it rather than carrying a
    // style per state of its own (each style on an object costs internal SRAM).
    init(s.tile);
    fill(s.tile, color::Surface);
    ink(s.tile, color::Ink);
    lv_style_set_radius(&s.tile, 14);
    init(s.tile_on);
    bg(s.tile_on, color::TileOn);
    ink(s.tile_on, color::TileOnInk);
    init(s.tile_offline);
    ink(s.tile_offline, color::InkDim);
    init(s.tile_on_pressed);
    bg(s.tile_on_pressed, color::TileOnPressed);
    init(s.glyph);
    round(s.glyph);
    fill(s.glyph, color::Raised);
    ink(s.glyph, color::InkDim);
    center(s.glyph);
    lv_style_set_text_font(&s.glyph, &font_home_icons_24);
    init(s.glyph_on);
    bg(s.glyph_on, color::Amber);
    ink(s.glyph_on, color::OnAmber);
    init(s.glyph_off);
    bg(s.glyph_off, color::Surface);
    ink(s.glyph_off, color::InkFaint);
    init(s.state);
    ink(s.state, color::InkDim);
    init(s.state_on);
    ink(s.state_on, color::TileOnState);
    init(s.state_off);
    ink(s.state_off, color::Bad);
    init(s.step);
    fill(s.step, color::Raised);
    ink(s.step, color::Ink);
    lv_style_set_radius(&s.step, 10);
    center(s.step);
    init(s.step_on);
    bg(s.step_on, color::StepOn);
    ink(s.step_on, color::TileOnInk);
    init(s.step_on_pressed);
    bg(s.step_on_pressed, color::StepOnPressed);
    init(s.pager);
    round(s.pager);
    fill(s.pager, color::Raised);
    ink(s.pager, color::Ink);
    center(s.pager);
    init(s.tile_stale);
    bg(s.tile_stale, color::Surface);
    ink(s.tile_stale, color::InkDim);
    init(s.glyph_stale);
    bg(s.glyph_stale, color::Raised);
    ink(s.glyph_stale, color::InkFaint);
    init(s.glyph_on_stale);
    bg(s.glyph_on_stale, color::StaleOnEdge);
    ink(s.glyph_on_stale, color::StaleOnInk);
    init(s.state_on_stale);
    ink(s.state_on_stale, color::StaleOnInk);
    init(s.empty_icon);
    ink(s.empty_icon, color::InkFaint);
    lv_style_set_text_font(&s.empty_icon, &font_home_icons_48);
    center(s.empty_icon);

    // Bottom bar: the status line. Its disc says what kind of news it is;
    // the words are the news, then who said it and when.
    init(s.status_text);
    ink(s.status_text, color::InkDim);
    init(s.status_voice);
    ink(s.status_voice, color::Ink);
    init(s.status_detail);
    ink(s.status_detail, color::InkFaint);
    init(s.disc);
    round(s.disc);
    fill(s.disc, color::CyanTint);
    ink(s.disc, color::Cyan);
    center(s.disc);
    lv_style_set_text_font(&s.disc, &font_home_icons_24);
    init(s.disc_busy);
    bg(s.disc_busy, color::Indigo);
    ink(s.disc_busy, 0xFFFFFF);
    init(s.disc_ok);
    bg(s.disc_ok, color::OkTint);
    ink(s.disc_ok, color::Ok);
    init(s.disc_warn);
    bg(s.disc_warn, color::WarnTint);
    ink(s.disc_warn, color::Warn);
    init(s.disc_bad);
    bg(s.disc_bad, color::BadTint);
    ink(s.disc_bad, color::Bad);
    init(s.disc_muted);
    bg(s.disc_muted, color::Raised);
    ink(s.disc_muted, color::InkDim);

    // The choice tray, over the grid, when a spoken command needs a pick.
    init(s.card);
    fill(s.card, color::CardBg);
    lv_style_set_radius(&s.card, 16);
    lv_style_set_border_width(&s.card, 1);
    lv_style_set_border_color(&s.card, lv_color_hex(color::CardEdge));
    lv_style_set_pad_all(&s.card, 12);
    lv_style_set_pad_row(&s.card, 10);
    lv_style_set_pad_column(&s.card, 10);
    init(s.choice);
    round(s.choice);
    fill(s.choice, color::Pressed);
    lv_style_set_border_width(&s.choice, 1);
    lv_style_set_border_color(&s.choice, lv_color_hex(color::Cyan));
    lv_style_set_pad_hor(&s.choice, 18);
    init(s.choice_pressed);
    bg(s.choice_pressed, color::CyanDeep);

    // System page.
    init(s.hero);
    round(s.hero);
    fill(s.hero, color::CyanTint);
    ink(s.hero, color::Cyan);
    center(s.hero);
    lv_style_set_text_font(&s.hero, &font_home_icons_48);
    init(s.hero_attention);
    bg(s.hero_attention, color::WarnTint);
    ink(s.hero_attention, color::Warn);
    init(s.hero_error);
    bg(s.hero_error, color::BadTint);
    ink(s.hero_error, color::Bad);
    init(s.sys_title);
    ink(s.sys_title, color::Ink);
    center(s.sys_title);
    init(s.bar_bg);
    fill(s.bar_bg, color::Raised);
    round(s.bar_bg);
    init(s.bar_ind);
    fill(s.bar_ind, color::Cyan);
    round(s.bar_ind);
    init(s.sys_action);
    round(s.sys_action);
    fill(s.sys_action, color::Raised);
    ink(s.sys_action, color::Ink);
    lv_style_set_pad_hor(&s.sys_action, 24);
    init(s.sys_ghost);
    round(s.sys_ghost);
    lv_style_set_border_width(&s.sys_ghost, 1);
    lv_style_set_border_color(&s.sys_ghost, lv_color_hex(color::CardEdge));
    ink(s.sys_ghost, color::InkDim);
    lv_style_set_pad_hor(&s.sys_ghost, 18);
}

void HomePanelView::Build(const BuildContext& ctx) {
    if (built_) return;
    display_ = ctx.display;
    font_ = ctx.font;
    icon_font_ = ctx.icon_font != nullptr ? ctx.icon_font : ctx.font;
    BuildStyles();
    RefreshPanelFont();
    lv_style_set_text_font(&S().root, &panel_font_);
    lv_style_set_text_font(&S().net, icon_font_);

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

    // Toasts sit in the top bar's free middle, over nothing a person reads.
    toast_ = NewLabel(root_, &S().toast);
    lv_obj_align(toast_, LV_ALIGN_TOP_MID, 0, (kTopH - 34) / 2);
    Visible(toast_, false);

    built_ = true;
    ApplyFontMetrics();
    RebuildNavigation();
    RenderScene();
    timer_ = lv_timer_create(OnTimer, 1000, this);
}

// The text font, then the inline icons, then the Chinese the builtin subset
// lacks, then whatever the text font already fell back to. The generated fonts
// are const tables, so the chain runs through copies of them.
void HomePanelView::RefreshPanelFont() {
    static lv_font_t icons;
    static lv_font_t text;
    text = font_home_text_20;
    text.fallback = font_->fallback;
    icons = font_home_icons_20;
    icons.fallback = &text;
    panel_font_ = *font_;
    panel_font_.fallback = &icons;
}

void HomePanelView::BuildTopBar() {
    top_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(top_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_size(top_, kWidth, kTopH);

    lv_obj_t* left = NewBox(top_);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(left, 14, 0);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 20, 0);
    clock_label_ = NewLabel(left, &S().clock);
    lv_label_set_text_static(clock_label_, "--:--");
    home_label_ = NewLabel(left, &S().dim_text);

    lv_obj_t* right = NewBox(top_);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 10, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -20, 0);
    conn_label_ = NewLabel(right, &S().pill);
    lv_obj_add_style(conn_label_, &S().pill_bad, LV_STATE_USER_1);
    net_icon_ = NewLabel(right, &S().net);
    conn_dot_ = NewBox(right, &S().dot);
    lv_obj_set_size(conn_dot_, 8, 8);
}

void HomePanelView::BuildNav() {
    nav_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(nav_, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_pos(nav_, 0, kTopH);
    lv_obj_set_size(nav_, kNavW, kMidH);
    lv_obj_set_style_pad_ver(nav_, 8, 0);
    lv_obj_set_style_pad_hor(nav_, 8, 0);
    lv_obj_set_style_pad_row(nav_, 2, 0);
    lv_obj_set_flex_flow(nav_, LV_FLEX_FLOW_COLUMN);
    // A registry can hold up to 32 areas; the column scrolls when it must.
    lv_obj_add_flag(nav_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nav_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav_, LV_SCROLLBAR_MODE_OFF);
}

// One row per area: a fixed height holds the name to a single line (three
// characters fit, a longer name ends in dots) and the device count sits in
// the right padding.
HomePanelView::NavSlot& HomePanelView::NavSlotAt(size_t index) {
    while (nav_slots_.size() <= index) {
        NavSlot slot;
        slot.item = NewLabel(nav_, &S().nav);
        lv_obj_add_style(slot.item, &S().nav_on, LV_STATE_CHECKED);
        lv_obj_set_size(slot.item, kNavW - 16, kNavItemH);
        lv_label_set_long_mode(slot.item, LV_LABEL_LONG_MODE_DOTS);
        Touchable(slot.item, Tag(static_cast<uint8_t>(Role::Nav), nav_slots_.size()));
        slot.count = NewLabel(slot.item, &S().count);
        lv_obj_align(slot.count, LV_ALIGN_RIGHT_MID, kNavCountPad - 6, 0);
        nav_slots_.push_back(slot);
    }
    return nav_slots_[index];
}

void HomePanelView::BuildGrid() {
    grid_ = NewBox(dash_);
    lv_obj_set_pos(grid_, kNavW, kTopH);
    lv_obj_set_size(grid_, kGridW, kMidH);
    for (size_t i = 0; i < kTileSlots; ++i) {
        TileSlot& t = tiles_[i];
        const int column = static_cast<int>(i) % kCols;
        const int row = static_cast<int>(i) / kCols;
        t.box = NewBox(grid_, &S().tile);
        lv_obj_add_style(t.box, &S().tile_on, LV_STATE_CHECKED);
        lv_obj_add_style(t.box, &S().pressed, LV_STATE_PRESSED);
        lv_obj_add_style(t.box, &S().tile_on_pressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_add_style(t.box, &S().tile_stale, LV_STATE_DISABLED);
        lv_obj_add_style(t.box, &S().tile_offline, LV_STATE_USER_1);
        lv_obj_set_pos(t.box, kGridPad + column * (kTileW + kGap), kGridPad + row * (kTileH + kGap));
        lv_obj_set_size(t.box, kTileW, kTileH);
        Touchable(t.box, Tag(static_cast<uint8_t>(Role::Tile), i));
        t.glyph = NewLabel(t.box, &S().glyph);
        lv_obj_add_style(t.glyph, &S().glyph_on, LV_STATE_CHECKED);
        lv_obj_add_style(t.glyph, &S().glyph_stale, LV_STATE_DISABLED);
        lv_obj_add_style(t.glyph, &S().glyph_on_stale, LV_STATE_CHECKED | LV_STATE_DISABLED);
        lv_obj_add_style(t.glyph, &S().glyph_off, LV_STATE_USER_1);
        t.name = NewLabel(t.box);
        lv_label_set_long_mode(t.name, LV_LABEL_LONG_MODE_DOTS);
        t.state = NewLabel(t.box, &S().state);
        lv_obj_add_style(t.state, &S().state_on, LV_STATE_CHECKED);
        lv_obj_add_style(t.state, &S().state_on_stale, LV_STATE_CHECKED | LV_STATE_DISABLED);
        lv_obj_add_style(t.state, &S().state_off, LV_STATE_USER_1);
        lv_label_set_long_mode(t.state, LV_LABEL_LONG_MODE_DOTS);
        for (lv_obj_t** step : {&t.minus, &t.plus}) {
            *step = NewLabel(t.box, &S().step);
            lv_obj_add_style(*step, &S().pressed, LV_STATE_PRESSED);
            lv_obj_add_style(*step, &S().step_on, LV_STATE_CHECKED);
            lv_obj_add_style(*step, &S().step_on_pressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
        }
        lv_label_set_text_static(t.minus, HOME_ICON_REMOVE);
        Touchable(t.minus, Tag(static_cast<uint8_t>(Role::Minus), i));
        lv_label_set_text_static(t.plus, HOME_ICON_ADD);
        Touchable(t.plus, Tag(static_cast<uint8_t>(Role::Plus), i));
        Visible(t.box, false);
    }

    empty_icon_ = NewLabel(grid_, &S().empty_icon);
    lv_obj_set_width(empty_icon_, kGridW);
    lv_obj_align(empty_icon_, LV_ALIGN_CENTER, 0, -44);
    empty_label_ = NewLabel(grid_, &S().dim_center);
    lv_obj_set_width(empty_label_, kGridW - 80);
    lv_label_set_long_mode(empty_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(empty_label_, LV_ALIGN_CENTER, 0, 20);

    page_prev_ = NewLabel(grid_, &S().pager);
    lv_obj_add_style(page_prev_, &S().pressed, LV_STATE_PRESSED);
    lv_label_set_text_static(page_prev_, HOME_ICON_CHEVRON_LEFT);
    lv_obj_set_size(page_prev_, kPagerW, kPagerH);
    lv_obj_set_pos(page_prev_, kGridW / 2 - 48 - kPagerW, kPagerY);
    Touchable(page_prev_, Tag(static_cast<uint8_t>(Role::PagePrev), 0));
    page_label_ = NewLabel(grid_, &S().dim_center);
    lv_obj_set_width(page_label_, 96);
    page_next_ = NewLabel(grid_, &S().pager);
    lv_obj_add_style(page_next_, &S().pressed, LV_STATE_PRESSED);
    lv_label_set_text_static(page_next_, HOME_ICON_CHEVRON_RIGHT);
    lv_obj_set_size(page_next_, kPagerW, kPagerH);
    lv_obj_set_pos(page_next_, kGridW / 2 + 48, kPagerY);
    Touchable(page_next_, Tag(static_cast<uint8_t>(Role::PageNext), 0));
}

void HomePanelView::BuildBottomBar() {
    bottom_ = NewBox(dash_, &S().bar);
    lv_obj_set_style_border_side(bottom_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_pos(bottom_, 0, kHeight - kBottomH);
    lv_obj_set_size(bottom_, kWidth, kBottomH);
    lv_obj_set_style_pad_left(bottom_, 20, 0);
    lv_obj_set_style_pad_right(bottom_, 12, 0);
    lv_obj_set_style_pad_column(bottom_, 12, 0);
    lv_obj_set_flex_flow(bottom_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    status_icon_ = NewLabel(bottom_, &S().disc);
    lv_obj_add_style(status_icon_, &S().disc_busy, LV_STATE_USER_1);
    lv_obj_add_style(status_icon_, &S().disc_ok, LV_STATE_USER_2);
    lv_obj_add_style(status_icon_, &S().disc_warn, LV_STATE_USER_3);
    lv_obj_add_style(status_icon_, &S().disc_bad, LV_STATE_USER_4);
    lv_obj_add_style(status_icon_, &S().disc_muted, LV_STATE_DISABLED);
    lv_obj_set_size(status_icon_, kStatusDisc, kStatusDisc);
    Visible(status_icon_, false);
    status_text_ = NewLabel(bottom_, &S().status_text);
    lv_obj_add_style(status_text_, &S().status_voice, LV_STATE_USER_1);
    lv_label_set_long_mode(status_text_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(status_text_, kStatusTextMaxW, 0);
    status_detail_ = NewLabel(bottom_, &S().status_detail);
    lv_label_set_long_mode(status_detail_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(status_detail_, 1);

    // The microphone ends the bar, under the thumb, at a size a finger finds.
    mic_ = NewLabel(bottom_, &S().mic);
    lv_obj_add_style(mic_, &S().mic_live, LV_STATE_CHECKED);
    lv_obj_add_style(mic_, &S().mic_busy, LV_STATE_USER_1);
    lv_obj_add_style(mic_, &S().mic_opening, LV_STATE_USER_2);
    lv_obj_add_style(mic_, &S().pressed, LV_STATE_PRESSED);
    lv_obj_set_size(mic_, kMic, kMic);
    lv_label_set_text_static(mic_, HOME_ICON_MIC);
    Touchable(mic_, Tag(static_cast<uint8_t>(Role::Mic), 0));
}

// The words of a spoken command's result live in the status line; the card is
// only the tray of choices when the command named more than one device.
void HomePanelView::BuildCard() {
    card_ = NewBox(root_, &S().card);
    lv_obj_set_size(card_, kGridW - 2 * kGridPad, LV_SIZE_CONTENT);
    lv_obj_align(card_, LV_ALIGN_BOTTOM_LEFT, kCardX, -(kBottomH + 10));
    lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_ROW_WRAP);
    // The tray swallows taps (they must not reach the tile underneath) and a
    // tap on its body dismisses it early; the choices stay tappable.
    Touchable(card_, Tag(static_cast<uint8_t>(Role::Card), 0));
    for (size_t i = 0; i < kCandidateSlots; ++i) {
        candidates_[i] = NewLabel(card_, &S().choice);
        lv_obj_add_style(candidates_[i], &S().choice_pressed, LV_STATE_PRESSED);
        Touchable(candidates_[i], Tag(static_cast<uint8_t>(Role::Candidate), i));
        Visible(candidates_[i], false);
    }
    Visible(card_, false);
}

void HomePanelView::BuildSystemPage() {
    sys_ = NewBox(root_);
    lv_obj_set_size(sys_, kWidth, kHeight);
    lv_obj_t* column = NewBox(sys_);
    lv_obj_set_size(column, kWidth - 160, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(column, LV_ALIGN_CENTER, 0, -20);
    sys_icon_ = NewLabel(column, &S().hero);
    lv_obj_add_style(sys_icon_, &S().hero_attention, LV_STATE_USER_1);
    lv_obj_add_style(sys_icon_, &S().hero_error, LV_STATE_USER_2);
    lv_obj_set_size(sys_icon_, kHeroDisc, kHeroDisc);
    lv_obj_set_style_pad_top(sys_icon_, PadFor(kHeroDisc, lv_font_get_line_height(&font_home_icons_48)), 0);
    lv_obj_set_style_margin_bottom(sys_icon_, 22, 0);
    sys_title_ = NewLabel(column, &S().sys_title);
    lv_obj_set_width(sys_title_, lv_pct(100));
    lv_obj_set_style_margin_bottom(sys_title_, 8, 0);
    sys_detail_ = NewLabel(column, &S().dim_center);
    lv_obj_set_width(sys_detail_, lv_pct(100));
    lv_label_set_long_mode(sys_detail_, LV_LABEL_LONG_MODE_WRAP);
    sys_bar_ = lv_bar_create(column);
    lv_obj_remove_style_all(sys_bar_);
    lv_obj_add_style(sys_bar_, &S().bar_bg, LV_PART_MAIN);
    lv_obj_add_style(sys_bar_, &S().bar_ind, LV_PART_INDICATOR);
    lv_obj_set_size(sys_bar_, 360, 8);
    lv_obj_set_style_margin_top(sys_bar_, 18, 0);
    lv_bar_set_range(sys_bar_, 0, 100);
    sys_action_ = NewLabel(column, &S().sys_action);
    lv_obj_set_style_margin_top(sys_action_, 24, 0);
    Touchable(sys_action_, Tag(static_cast<uint8_t>(Role::Primary), 0));
    sys_setup_ = NewLabel(sys_, &S().sys_ghost);
    Text(sys_setup_, std::string(HOME_ICON_SETTINGS "  ") + home_copy::kSetupAction);
    lv_obj_align(sys_setup_, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    Touchable(sys_setup_, Tag(static_cast<uint8_t>(Role::Setup), 0));
    Visible(sys_bar_, false);
    Visible(sys_action_, false);
    Visible(sys_setup_, false);
    Visible(sys_, false);
}

// Everything that depends on the text font's metrics. Labels are not
// containers: fixed-height "buttons" center their text with padding.
void HomePanelView::ApplyFontMetrics() {
    if (!built_) return;
    Styles& s = S();
    const int line = lv_font_get_line_height(&panel_font_);
    const int icon_line = lv_font_get_line_height(&font_home_icons_24);
    lv_style_set_pad_ver(&s.nav, PadFor(kNavItemH, line));
    lv_style_set_pad_ver(&s.choice, PadFor(40, line));
    lv_style_set_pad_ver(&s.toast, PadFor(34, line));
    lv_style_set_pad_ver(&s.pill, PadFor(30, line));
    lv_style_set_pad_ver(&s.sys_action, PadFor(44, line));
    lv_style_set_pad_ver(&s.sys_ghost, PadFor(44, line));
    lv_style_set_pad_top(&s.mic, PadFor(kMic, icon_line));
    lv_style_set_pad_top(&s.disc, PadFor(kStatusDisc, icon_line));
    lv_style_set_pad_top(&s.pager, PadFor(kPagerH, line));
    lv_style_set_pad_top(&s.glyph, PadFor(kDisc, icon_line));
    lv_style_set_pad_top(&s.step, PadFor(kStepH, line));
    lv_obj_report_style_change(nullptr);

    // Icon disc and steps share the first row; name and state span the tile.
    for (TileSlot& t : tiles_) {
        lv_obj_set_pos(t.glyph, kTilePad, kTilePad);
        lv_obj_set_size(t.glyph, kDisc, kDisc);
        const int step_y = kTilePad + (kDisc - kStepH) / 2;
        lv_obj_set_pos(t.plus, kTileW - kTilePad - kStepW, step_y);
        lv_obj_set_size(t.plus, kStepW, kStepH);
        lv_obj_set_pos(t.minus, kTileW - kTilePad - 2 * kStepW - 6, step_y);
        lv_obj_set_size(t.minus, kStepW, kStepH);
        const int name_y = kTileH - kTilePad + 2 - 2 * (line - 2);
        lv_obj_set_pos(t.name, kTilePad + 2, name_y);
        lv_obj_set_size(t.name, kTileW - 2 * kTilePad - 2, line);
        lv_obj_set_pos(t.state, kTilePad + 2, name_y + line - 2);
        lv_obj_set_size(t.state, kTileW - 2 * kTilePad - 2, line);
    }
    lv_obj_set_height(status_text_, line);
    lv_obj_set_height(status_detail_, line);
    lv_obj_set_pos(page_label_, kGridW / 2 - 48, kPagerY + PadFor(kPagerH, line));
    rendered_generation_ = UINT32_MAX;  // widths changed; re-measure state texts
}

void HomePanelView::SetFont(const lv_font_t* font) {
    if (font == nullptr || font == font_ || !built_) return;
    font_ = font;
    RefreshPanelFont();
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

void HomePanelView::SetNetworkIcon(const char* icon) {
    ViewLock lock(display_);
    if (icon == nullptr || !built_) return;
    TextStatic(net_icon_, icon);
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


// Device names are free text typed on the phone, so a name can hold a
// character the panel font lacks; say so instead of silently dropping it.
void HomePanelView::LogMissingGlyphs() const {
    if (!built_) return;
    size_t missing = 0;
    uint32_t example = 0;
    const std::string* where = nullptr;
    const auto scan = [&](const std::string& text) {
        const char* p = text.c_str();
        for (uint32_t cp = NextCodePoint(p); cp != 0; cp = NextCodePoint(p)) {
            lv_font_glyph_dsc_t dsc;
            if (cp < 0x20 || lv_font_get_glyph_dsc(&panel_font_, &dsc, cp, 0)) continue;
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
    // The scene and its severity pick the picture; the words are this
    // panel's Chinese for what the projector said.
    Text(sys_title_, home_copy::Title(model.scene, model.status_text));
    const int percent = model.scene == UiScene::Updating ? home_copy::ProgressPercent(model.detail_text) : -1;
    Text(sys_detail_, home_copy::Text(model.detail_text));
    TextStatic(sys_icon_, home_icons::Hero(model.scene));
    SetState(sys_icon_, LV_STATE_USER_1, model.severity == UiSeverity::Attention);
    SetState(sys_icon_, LV_STATE_USER_2, model.severity == UiSeverity::Error);
    Visible(sys_bar_, percent >= 0);
    if (percent >= 0 && lv_bar_get_value(sys_bar_) != percent) lv_bar_set_value(sys_bar_, percent, LV_ANIM_OFF);
    sys_intent_ = touch && (model.primary_intent == UiIntent::OpenConversation ||
                            model.primary_intent == UiIntent::ToggleMicrophone)
        ? model.primary_intent : UiIntent::None;
    if (sys_intent_ != UiIntent::None) {
        Text(sys_action_, home_copy::Text(model.primary_label));
    } else if (model.primary_presentation == UiActionPresentation::InputHint) {
        Text(sys_action_, home_copy::Text(model.input_hint));
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
        rendered_generation_ = store_.generation();
    }
    // A stale cache is drawn dark and inert, tile by tile: a whole-grid
    // opacity washed the on tiles' words into their background.
    const bool stale = Stale();
    for (TileSlot& t : tiles_) {
        for (lv_obj_t* part : {t.box, t.glyph, t.state}) SetState(part, LV_STATE_DISABLED, stale);
        Visible(t.minus, t.steps && !stale);
        Visible(t.plus, t.steps && !stale);
    }
    RenderStatusLine();
    RenderCard();
}

void HomePanelView::RenderTopBar() {
    Text(home_label_, store_.has_snapshot() ? store_.home().home_name : std::string("我的家"));

    // The dot and the pill answer "is this panel connected to Eidolon": the
    // dashboard is only on screen once it is, so connected is the quiet case
    // (a green dot) and only a lost link earns words. Whether the home has
    // synced is the grid's business (its empty state and dimming).
    const char* link = nullptr;
    bool bad = false;
    if (scene_ == UiScene::Reconnecting) {
        link = HOME_ICON_SYNC " 重连中";
    } else if (Stale()) {
        link = HOME_ICON_CLOUD_OFF " 离线 · 显示上次状态";
        bad = true;
    }
    if (link != nullptr) TextStatic(conn_label_, link);
    Visible(conn_label_, link != nullptr);
    SetState(conn_label_, LV_STATE_USER_1, bad);
    Visible(conn_dot_, link == nullptr);
    Text(clock_label_, LocalClock());
}

void HomePanelView::RenderMic() {
    const bool conversation = scene_ == UiScene::Conversation;
    const bool live = conversation && !mic_muted_ &&
        (IsTalking(turn_) || (turn_ == TurnPhase::Idle && !IsPushToTalk(mode_)));
    SetState(mic_, LV_STATE_CHECKED, live);
    SetState(mic_, LV_STATE_USER_1, conversation && IsBusy(turn_));
    SetState(mic_, LV_STATE_USER_2, scene_ == UiScene::OpeningConversation);
    const char* icon = HOME_ICON_MIC;
    if (conversation && mic_muted_) icon = HOME_ICON_MIC_OFF;
    else if (conversation && IsBusy(turn_)) icon = HOME_ICON_SPARKLE;
    else if (scene_ == UiScene::OpeningConversation) icon = HOME_ICON_SYNC;
    TextStatic(mic_, icon);
}

// The line left of the microphone. A spoken command's progress and result
// come first (the panel has no speaker: this is the answer), then what the
// voice session is waiting for, then the home's latest change.
void HomePanelView::RenderStatusLine() {
    const auto quoted = [](const std::string& words) { return words.empty() ? std::string() : "「" + words + "」"; };
    const std::string heard = subtitle_from_user_ ? quoted(subtitle_) : std::string();
    const char* icon = nullptr;
    lv_state_t tone = LV_STATE_DEFAULT;  // a cyan disc: listening, or a question
    std::string text;
    std::string detail;
    if (card_mode_ == CardMode::Result) {
        switch (smarthome::OutcomeTone(result_.outcome)) {
        case smarthome::ResultTone::Normal: {
            const bool question = result_.outcome == smarthome::VoiceOutcome::Ambiguous ||
                result_.outcome == smarthome::VoiceOutcome::Clarification;
            icon = question ? HOME_ICON_QUESTION : HOME_ICON_CHECK_CIRCLE;
            tone = question ? LV_STATE_DEFAULT : LV_STATE_USER_2;
            break;
        }
        case smarthome::ResultTone::Attention:
            icon = HOME_ICON_WARNING;
            tone = LV_STATE_USER_3;
            break;
        case smarthome::ResultTone::Error:
            icon = HOME_ICON_ERROR;
            tone = LV_STATE_USER_4;
            break;
        }
        text = result_.message;
        detail = quoted(result_.utterance);
    } else if (card_mode_ == CardMode::Listening) {
        icon = HOME_ICON_MIC;
        text = "正在聆听…";
        detail = heard;
    } else if (card_mode_ == CardMode::Processing) {
        icon = HOME_ICON_SPARKLE;
        tone = LV_STATE_USER_1;
        text = "正在理解…";
        detail = heard;
    } else if (scene_ == UiScene::OpeningConversation) {
        icon = HOME_ICON_SYNC;
        text = "正在连接…";
    } else if (scene_ == UiScene::Conversation) {
        if (mic_muted_) {
            icon = HOME_ICON_MIC_OFF;
            tone = LV_STATE_DISABLED;
            text = "麦克风已关闭";
        } else if (turn_ == TurnPhase::AgentSpeaking) {
            icon = HOME_ICON_SPARKLE;
            tone = LV_STATE_USER_1;
            text = "正在理解…";
        } else {
            icon = HOME_ICON_MIC;
            text = IsPushToTalk(mode_) ? "按住麦克风说话" : "请说出指令";
        }
    } else {
        const auto lines = smarthome::FormatActivity(store_.activity(), store_.home().utc_offset_minutes);
        text = lines.what;
        detail = lines.detail;
        if (text.empty() && mic_intent_ == UiIntent::OpenConversation) text = "轻点麦克风，说出指令";
    }
    Visible(status_icon_, icon != nullptr);
    if (icon != nullptr) TextStatic(status_icon_, icon);
    for (lv_state_t state : {LV_STATE_USER_1, LV_STATE_USER_2, LV_STATE_USER_3, LV_STATE_USER_4, LV_STATE_DISABLED}) {
        SetState(status_icon_, state, tone == state);
    }
    SetState(status_text_, LV_STATE_USER_1, icon != nullptr);
    Text(status_text_, text);
    Text(status_detail_, detail);
}

// The panel shows its own area until the person picks one, so the first
// snapshot (and any later move of the panel to another area) takes effect; a
// picked area is kept for as long as the home still has it.
void HomePanelView::RebuildNavigation() {
    nav_entries_ = smarthome::BuildNav(store_.home());
    const bool kept = std::any_of(nav_entries_.begin(), nav_entries_.end(),
                                  [&](const smarthome::NavEntry& e) { return e.area_id == area_; });
    if (!kept) area_chosen_ = false;
    if (!area_chosen_) {
        const std::string area = smarthome::DefaultArea(store_.home());
        if (area != area_) {
            area_ = area;
            page_ = 0;
        }
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
    const int text_width = kTileW - 2 * kTilePad - 2;
    for (size_t i = 0; i < devices.size() && slot < kTileSlots; ++i) {
        const smarthome::Device& device = devices[i];
        if (!smarthome::InArea(device, area_) || ordinal++ < first) continue;
        TileSlot& t = tiles_[slot++];
        t.device = static_cast<int>(i);
        TextStatic(t.glyph, home_icons::Device(smarthome::IconFor(device)));
        Text(t.name, device.name);
        std::string state = smarthome::DeviceStateText(device);
        lv_point_t size;
        lv_text_get_size(&size, state.c_str(), &panel_font_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > text_width) state = smarthome::DeviceStateText(device, true);
        Text(t.state, state);
        const bool active = smarthome::IsActive(device);
        const bool offline = !device.online || !device.has_state;
        for (lv_obj_t* part : {t.box, t.glyph, t.state, t.minus, t.plus}) SetState(part, LV_STATE_CHECKED, active);
        for (lv_obj_t* part : {t.box, t.glyph, t.state}) SetState(part, LV_STATE_USER_1, offline);
        t.steps = smarthome::OffersSteps(device);
        Visible(t.box, true);
    }
    for (; slot < kTileSlots; ++slot) {
        tiles_[slot].device = -1;
        Visible(tiles_[slot].box, false);
    }

    if (!store_.has_snapshot()) {
        TextStatic(empty_icon_, HOME_ICON_CLOUD_OFF);
        TextStatic(empty_label_, "家居数据尚未同步\n正在向主机请求…");
    } else if (devices.empty()) {
        TextStatic(empty_icon_, HOME_ICON_HOME);
        TextStatic(empty_label_, "还没有家居设备\n请在手机 Eidolon「智能家居」里添加");
    }
    Visible(empty_icon_, devices.empty());
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


void HomePanelView::ShowResult(smarthome::VoiceResult&& result) {
    result_ = std::move(result);
    card_mode_ = CardMode::Result;
    const bool choices = result_.outcome == smarthome::VoiceOutcome::Ambiguous && result_.has_command;
    card_until_ms_ = MonoMs() + (choices ? kChoiceResultMs : kResultMs);
}

void HomePanelView::HideCard() {
    card_mode_ = CardMode::Hidden;
    RenderCard();
    if (DashboardVisible()) RenderStatusLine();
}

void HomePanelView::RenderCard() {
    const bool choices = card_mode_ == CardMode::Result && result_.outcome == smarthome::VoiceOutcome::Ambiguous &&
        result_.has_command;
    size_t shown = 0;
    for (size_t i = 0; i < kCandidateSlots; ++i) {
        const bool used = choices && i < result_.candidates.size();
        Visible(candidates_[i], used);
        if (used) {
            Text(candidates_[i], result_.candidates[i].name);
            ++shown;
        }
    }
    Visible(card_, shown > 0 && DashboardVisible());
}

void HomePanelView::OnTimer(lv_timer_t* timer) {
    static_cast<HomePanelView*>(lv_timer_get_user_data(timer))->Tick();
}

// LVGL task, lock held. Cheap by construction: string compares, no layout.
void HomePanelView::Tick() {
    const int64_t now = MonoMs();
    if (store_.link_up() && (!store_.has_snapshot() || store_.awaiting_snapshot())) {
        RequestSync();
    }
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
        if (index < nav_entries_.size()) area_chosen_ = true;
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
