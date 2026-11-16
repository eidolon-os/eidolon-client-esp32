#pragma once
#include <lvgl.h>
#include <memory>
#include <functional>
#include <string>
#include "eidolon/eidolon_view.h"
#include "eidolon/expression/core/runtime.h"
#if CONFIG_EIDOLON_PET_FACE
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "eidolon/pet/pet_runtime.h"
#include "pet_face_skin.h"
#endif
class Display;
namespace stackchan::avatar { class DefaultAvatar; }
namespace eidolon {
// Shared product surface; all animation state is owned by the LVGL task/lock.
class CompanionFaceView : public EidolonView, public expression::Surface {
public:
    struct BuildContext { lv_obj_t* parent; const lv_font_t* font; Display* display; const lv_font_t* icon_font; };
    CompanionFaceView();
    ~CompanionFaceView() override;
    expression::Surface* Expressions() override { return this; }
    struct Indicators { lv_obj_t* microphone; lv_obj_t* network; lv_obj_t* battery; };
    Indicators indicators() const { return indicators_; }
    void Build(const BuildContext& ctx);
    void Render(const EidolonUiModel& model) override;
    // Caller holds the display lock and keeps the font alive until replacement.
    void SetContentFont(const lv_font_t* font);
    void SetSystemStatus(const char* text);
    void SetMotionDiagnostic(const char* name, const char* status, const char* reason) override;
    void ShowNotification(const char* text, int duration_ms);
    void PulseEmotion(const char* emotion, int ttl_ms);
    expression::Event Submit(const expression::Plan& plan) override;
    expression::Event Cancel(uint64_t token) override;
    // Called by the display on its LVGL timer, after completed SPI transfers.
    void Advance(uint32_t completed_frame);
    uint32_t prepared_frame() const { return prepared_frame_; }
    using Observer = std::function<void(expression::Event)>;
    void SetObserver(Observer observer) override;
#if CONFIG_EIDOLON_PET_FACE
    // Board sense sink; any task. Only enqueues.
    void PostSense(const pet::SenseEvent& event);
#endif
private:
#if CONFIG_EIDOLON_PET_FACE
    void BuildPet(const BuildContext& ctx);
    // Advances the pet's life. host_frame=true while a face-core-1 plan is on screen:
    // the skin then always produces a flush so the receipt has a frame boundary.
    void AdvancePet(uint64_t now, bool host_frame);
    void PetSenses(uint64_t now);
    void PetOutputs(uint64_t now);
    static void OnPetTouch(lv_event_t* event);
    PetFaceSkin pet_skin_;
    pet::Runtime pet_;
    pet::Phase pet_phase_ = pet::Phase::Ambient;
    QueueHandle_t pet_senses_ = nullptr;
    bool pet_sink_registered_ = false;
    bool pet_dark_ = false;
    bool pet_pointer_ = false;
    float pet_px_ = 160, pet_py_ = 120, pet_travel_ = 0;
    lv_point_t pet_last_point_{};
    uint64_t pet_press_ms_ = 0;
    uint64_t pet_next_power_ms_ = 0, pet_next_led_ms_ = 0, pet_last_show_ms_ = 0, pet_next_stats_ms_ = 0;
    // Frames and frame time per phase over the stats window (index = pet::Phase).
    uint32_t pet_phase_frames_[7] = {}, pet_seen_frames_ = 0;
    uint64_t pet_phase_us_[7] = {}, pet_seen_us_ = 0;
    bool pet_card_shown_ = false;
    bool pet_charging_ = false;
    int pet_battery_ = -1;
    pet::Mood pet_logged_mood_ = pet::Mood::Calm;
    // The status bar shows state only (the antenna tips carry the session state);
    // every action lives in a pull-down control panel with large targets: tap the
    // bar, or drag down from it. Outside taps or 5 s idle close it.
    struct PanelButton { lv_obj_t* obj = nullptr; lv_obj_t* icon = nullptr; UiIntent intent = UiIntent::None; };
    lv_obj_t* bar_hit_ = nullptr;     // transparent target over the whole bar
    lv_obj_t* handle_ = nullptr;      // grab handle at the top centre
    lv_obj_t* scrim_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    PanelButton call_btn_, mic_btn_, setup_btn_;
    UiIntent session_button_intent_ = UiIntent::None;
    bool pet_muted_ = false;
    bool panel_open_ = false;
    uint64_t panel_until_ = 0;
    int32_t bar_press_y_ = 0;
    void PetAction(pet::HostAction action);
    void RenderPetChrome(const EidolonUiModel& model, bool face);
    bool PanelHasActions() const;
    void OpenPanel();
    void ClosePanel();
    static void OnBar(lv_event_t* event);
    static void OnScrim(lv_event_t* event);
    static void OnPanelButton(lv_event_t* event);
#endif
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
    void Benchmark(uint64_t now);
    unsigned bench_submitted_=0,bench_completed_=0,bench_failed_=0;
    bool bench_done_=false;
#endif
    void Apply(const expression::FacePose& pose);
    void Emit(expression::Event event);
    void RefreshInformation();
    void RefreshDiagnostic();
    lv_obj_t* diagnostic_=nullptr;
    std::string face_diagnostic_, motion_diagnostic_;
    uint64_t diagnostic_until_=0, diagnostic_token_=0;
    void UpdateLayout();
    void RefreshSubtitle(uint64_t now);
    void ClearSubtitle();
    static void OnPrimary(lv_event_t* event);
    static void OnClose(lv_event_t* event);
    static void OnSetup(lv_event_t* event);
    Display* display_ = nullptr;
    std::unique_ptr<stackchan::avatar::DefaultAvatar> avatar_;
    expression::Runtime runtime_;
    Observer observer_;
    Indicators indicators_{};

    lv_obj_t* viewport_ = nullptr;
    lv_obj_t* detail_panel_ = nullptr;
    lv_obj_t* detail_ = nullptr;
    lv_obj_t* information_ = nullptr;
    lv_obj_t* caption_card_ = nullptr;
    lv_obj_t* subtitle_panel_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_obj_t* notification_ = nullptr;
    const lv_font_t* font_ = nullptr;
    std::string pending_subtitle_, displayed_subtitle_;
    uint64_t subtitle_updated_ = 0, subtitle_page_at_ = 0;
    uint64_t subtitle_idle_since_ = 0;
    bool subtitle_idle_ = false;
    bool dialogue_visible_ = false;
    bool agent_speaking_ = false;
    lv_obj_t* state_dot_ = nullptr;
    lv_obj_t* mode_ = nullptr;
    lv_obj_t* input_hint_ = nullptr;
    lv_obj_t* hint_action_ = nullptr;
    lv_obj_t* hint_key_ = nullptr;
    lv_obj_t* hint_effect_ = nullptr;
    bool has_actions_ = false;
    bool touch_navigation_ = false;
    uint64_t detail_scroll_start_ = 0;
    std::string information_text_;
    uint64_t notification_until_ = 0;
    bool has_model_ = false;
    bool talk_gesture_active_ = false;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* setup_ = nullptr;
    lv_obj_t* primary_ = nullptr;
    lv_obj_t* primary_label_ = nullptr;
    lv_obj_t* close_ = nullptr;
    UiIntent primary_intent_ = UiIntent::None;
    uint32_t prepared_frame_ = 0;
    bool awaiting_frame_ = false;
    uint64_t next_local_token_ = 1;
};
}
