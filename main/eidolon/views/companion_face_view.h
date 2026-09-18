#pragma once
#include <lvgl.h>
#include <memory>
#include <functional>
#include <string>
#include "eidolon/eidolon_view.h"
#include "eidolon/expression/core/runtime.h"
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
    void SetSystemStatus(const char* text);
    void ShowNotification(const char* text, int duration_ms);
    void PulseEmotion(const char* emotion, int ttl_ms);
    expression::Event Submit(const expression::Plan& plan) override;
    expression::Event Cancel(uint64_t token) override;
    // Called by the display on its LVGL timer, after completed SPI transfers.
    void Advance(uint32_t completed_frame);
    uint32_t prepared_frame() const { return prepared_frame_; }
    using Observer = std::function<void(expression::Event)>;
    void SetObserver(Observer observer) override;
private:
#if CONFIG_EIDOLON_COMPANION_BENCHMARK
    void Benchmark(uint64_t now);
    unsigned bench_submitted_=0,bench_completed_=0,bench_failed_=0;
    bool bench_done_=false;
#endif
    void Apply(const expression::FacePose& pose);
    void Emit(expression::Event event);
    void RefreshInformation();
    void UpdateLayout();
    static void OnPrimary(lv_event_t* event);
    static void OnClose(lv_event_t* event);
    static void OnSetup(lv_event_t* event);
    Display* display_ = nullptr;
    std::unique_ptr<stackchan::avatar::DefaultAvatar> avatar_;
    expression::Runtime runtime_;
    Observer observer_;
    Indicators indicators_{};
    lv_anim_t scroll_animation_{};
    lv_obj_t* viewport_ = nullptr;
    lv_obj_t* detail_panel_ = nullptr;
    lv_obj_t* detail_ = nullptr;
    lv_obj_t* information_ = nullptr;
    lv_obj_t* state_dot_ = nullptr;
    lv_obj_t* mode_ = nullptr;
    lv_obj_t* input_hint_ = nullptr;
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
