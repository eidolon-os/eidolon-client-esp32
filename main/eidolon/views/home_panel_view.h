#ifndef EIDOLON_HOME_PANEL_VIEW_H_
#define EIDOLON_HOME_PANEL_VIEW_H_

#include <lvgl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "eidolon/eidolon_view.h"
#include "eidolon/smarthome/smarthome_panel.h"
#include "eidolon/smarthome/smarthome_store.h"
#include "eidolon/smarthome/smarthome_tiles.h"
#include "eidolon/smarthome/smarthome_wire.h"

class Display;

namespace eidolon {

// Smart home panel for an 800x480 touch screen (korvo-1).
//
// Lifecycle scenes come from the shared EidolonUiModel and are drawn as a plain
// system page; Ready (and a live or recovering conversation) is the dashboard:
// area navigation, a page of device tiles, and a status line that carries a
// spoken command's progress and result (or else the latest activity), with a
// tray of choices when a command names more than one device. The home itself comes from the host through PanelSurface
// and is only ever a cache: tiles change when the host reports a change, never
// when the panel sends a command, and a stale cache is dimmed and inert.
//
// Every LVGL object is created once in Build() and reused. Tiles are a fixed
// pool of kTileSlots rebound to whichever devices the current area and page
// show; navigation items grow to the largest registry seen and are then reused.
// The look (Eidolon brand palette, Material icons, Chinese system copy) was
// reviewed against host renders: tests/home_panel_ui draws every state.
class HomePanelView : public EidolonView, public smarthome::PanelSurface {
public:
    struct BuildContext {
        lv_obj_t* parent = nullptr;          // active screen
        Display* display = nullptr;          // owns the LVGL lock
        const lv_font_t* font = nullptr;     // CJK text font
        const lv_font_t* icon_font = nullptr;  // FontAwesome subset (network icon)
    };

    HomePanelView();
    ~HomePanelView() override;

    // Caller holds the display lock (board SetupUI).
    void Build(const BuildContext& ctx);
    // Caller holds the display lock and keeps the font alive until replaced.
    void SetFont(const lv_font_t* font);
    void ShowNotification(const char* text, int duration_ms);

    // EidolonView (application task).
    void Render(const EidolonUiModel& model) override;
    smarthome::PanelSurface* SmartHomePanel() override { return this; }

    // smarthome::PanelSurface (application task).
    smarthome::ApplyOutcome Apply(smarthome::Message&& message) override;
    void SetLinkUp(bool up) override;
    void SetWallClock(int64_t utc_ms) override;
    void SetSink(smarthome::PanelSink* sink) override;

    // Network state for the top bar, as the board reports it (a static
    // FontAwesome string); fed on the legacy status bar's cadence.
    void SetNetworkIcon(const char* icon);

private:
    static constexpr size_t kTileSlots = 12;  // 4 columns x 3 rows
    static constexpr size_t kCandidateSlots = smarthome::kMaxCandidates;

    enum class Role : uint8_t {
        None, Tile, Minus, Plus, Nav, Candidate, Card, PagePrev, PageNext, Mic, Primary, Setup,
    };
    enum class CardMode : uint8_t { Hidden, Listening, Processing, Result };

    struct TileSlot {
        lv_obj_t* box = nullptr;
        lv_obj_t* glyph = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* state = nullptr;
        lv_obj_t* minus = nullptr;
        lv_obj_t* plus = nullptr;
        int device = -1;  // index into the held snapshot's devices
        bool steps = false;  // the device offers - / + (shown unless the cache is stale)
    };
    struct NavSlot {
        lv_obj_t* item = nullptr;
        lv_obj_t* count = nullptr;
    };

    class LoggingSink : public smarthome::PanelSink {
    public:
        void SendRequest(const std::string& request_json) override;
    };

    void BuildStyles();
    void BuildTopBar();
    void BuildNav();
    void BuildGrid();
    void BuildBottomBar();
    void BuildCard();
    void BuildSystemPage();
    void ApplyFontMetrics();
    void RefreshPanelFont();
    NavSlot& NavSlotAt(size_t index);

    void RenderScene();
    void RenderSystemPage();
    void RenderDashboard();
    void RenderTopBar();
    void RenderNav();
    void RenderGrid();
    void RenderMic();
    void RenderCard();
    void RenderStatusLine();
    void ShowResult(smarthome::VoiceResult&& result);
    void HideCard();
    void RebuildNavigation();
    void RequestSync();
    void Send(const smarthome::Command& command);
    std::string NextRequestId();
    std::string LocalClock() const;
    void LogMissingGlyphs() const;
    bool DashboardVisible() const;
    bool Stale() const;

    int64_t NowUtcMs() const;
    void Tick();
    static void OnTimer(lv_timer_t* timer);
    static void OnEvent(lv_event_t* event);
    void HandleEvent(lv_event_t* event);
    void HandleMic(lv_event_code_t code);

    Display* display_ = nullptr;
    const lv_font_t* font_ = nullptr;       // the board's text font
    const lv_font_t* icon_font_ = nullptr;  // FontAwesome (network icon)
    // The text font with the panel's Material icons as fallback, so one label
    // can carry an icon and its words; refreshed whenever the text font changes.
    lv_font_t panel_font_{};
    bool built_ = false;
    lv_timer_t* timer_ = nullptr;

    // Screen objects.
    lv_obj_t* root_ = nullptr;
    lv_obj_t* dash_ = nullptr;
    lv_obj_t* top_ = nullptr;
    lv_obj_t* home_label_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* net_icon_ = nullptr;
    lv_obj_t* conn_dot_ = nullptr;
    lv_obj_t* conn_label_ = nullptr;  // a pill, shown only when not connected
    lv_obj_t* mic_ = nullptr;
    lv_obj_t* nav_ = nullptr;
    lv_obj_t* grid_ = nullptr;
    lv_obj_t* empty_icon_ = nullptr;
    lv_obj_t* empty_label_ = nullptr;
    lv_obj_t* page_prev_ = nullptr;
    lv_obj_t* page_label_ = nullptr;
    lv_obj_t* page_next_ = nullptr;
    lv_obj_t* bottom_ = nullptr;
    lv_obj_t* status_icon_ = nullptr;    // what kind of news the status line carries
    lv_obj_t* status_text_ = nullptr;    // a command's result, the voice state, or the latest change
    lv_obj_t* status_detail_ = nullptr;  // what was heard, or when and by whom
    lv_obj_t* card_ = nullptr;           // the tray of choices
    lv_obj_t* toast_ = nullptr;
    lv_obj_t* sys_ = nullptr;
    lv_obj_t* sys_icon_ = nullptr;
    lv_obj_t* sys_title_ = nullptr;
    lv_obj_t* sys_bar_ = nullptr;
    lv_obj_t* sys_detail_ = nullptr;
    lv_obj_t* sys_action_ = nullptr;
    lv_obj_t* sys_setup_ = nullptr;
    std::array<TileSlot, kTileSlots> tiles_{};
    std::array<lv_obj_t*, kCandidateSlots> candidates_{};
    std::vector<NavSlot> nav_slots_;

    // Home cache and what the dashboard currently shows of it.
    smarthome::SmartHomeStore store_;
    std::vector<smarthome::NavEntry> nav_entries_;
    std::string area_;  // selected nav entry's area id
    bool area_chosen_ = false;  // picked on the panel, rather than the panel's own area
    size_t page_ = 0;
    uint32_t rendered_generation_ = UINT32_MAX;

    // Last projected lifecycle facts (copied; model strings are not owned).
    UiScene scene_ = UiScene::Starting;
    TurnPhase turn_ = TurnPhase::Idle;
    InteractionMode mode_ = CurrentInteractionMode();
    UiIntent mic_intent_ = UiIntent::None;      // the primary action (push-to-talk's talk key)
    UiIntent session_intent_ = UiIntent::None;  // what the microphone's tap does: the session button's click
    UiIntent sys_intent_ = UiIntent::None;
    bool talk_active_ = false;
    std::string subtitle_;
    bool subtitle_from_user_ = false;

    // Result card.
    CardMode card_mode_ = CardMode::Hidden;
    smarthome::VoiceResult result_;
    int64_t card_until_ms_ = 0;
    int64_t toast_until_ms_ = 0;

    // Wall clock: a trusted UTC reading advanced by the monotonic clock.
    int64_t clock_utc_ms_ = 0;
    int64_t clock_mono_ms_ = 0;

    LoggingSink logging_sink_;
    smarthome::PanelSink* sink_ = &logging_sink_;
    smarthome::RequestIdSource request_ids_;
    bool ids_seeded_ = false;  // reseeded on first use, once the radio feeds the RNG
};

}  // namespace eidolon

#endif  // EIDOLON_HOME_PANEL_VIEW_H_
