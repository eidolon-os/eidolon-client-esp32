// The StackChan control panel and the desk pet's local life, driven the way a
// person drives them: a finger on the screen, a tap on the head. Checks the two
// rules the view owns around a start or end:
//  - the panel's idle close never takes a button away from under a finger;
//  - while an end is drawn ahead of the runtime (the microphone may still be
//    open) the pet neither chirps nor moves its head on its own.
#include <lvgl.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

#include "board.h"
#include "display.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/output_policy.h"
#include "eidolon/pet/pet_chirp_player.h"
#include "eidolon/views/companion_face_view.h"

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_awesome_20_4);
using namespace eidolon;

namespace eidolon::pet {
int g_chirps = 0;
bool PlayChirp(Chirp, float) { ++g_chirps; return true; }
}  // namespace eidolon::pet

namespace {
lv_point_t g_point;
bool g_down = false;
void PointerRead(lv_indev_t*, lv_indev_data_t* data) {
    data->point = g_point;
    data->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// LVGL and the view's 20 ms display timer, for `ms` of device time.
void Run(CompanionFaceView& view, lv_indev_t* pointer, int ms) {
    for (int t = 0; t < ms; t += 20) {
        lv_tick_inc(20);
        lv_indev_read(pointer);
        lv_timer_handler();
        view.Advance(view.prepared_frame());
    }
}

void Collect(lv_obj_t* obj, std::vector<lv_obj_t*>& out) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        auto* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (lv_obj_get_width(child) == 84 && lv_obj_get_height(child) == 76 && lv_obj_is_visible(child))
            out.push_back(child);
        Collect(child, out);
    }
}
// The panel's buttons on screen (84 x 76, only those that apply), left to right.
std::vector<lv_obj_t*> PanelButtons(lv_obj_t* screen) {
    lv_obj_update_layout(screen);
    std::vector<lv_obj_t*> buttons;
    Collect(screen, buttons);
    std::sort(buttons.begin(), buttons.end(), [](lv_obj_t* a, lv_obj_t* b) {
        lv_area_t x, y;
        lv_obj_get_coords(a, &x);
        lv_obj_get_coords(b, &y);
        return x.x1 < y.x1;
    });
    return buttons;
}
lv_point_t Centre(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}
void Tap(lv_indev_t* pointer, lv_point_t at) {
    g_point = at;
    g_down = true;
    lv_indev_read(pointer);
    g_down = false;
    lv_indev_read(pointer);
}
pet::SenseEvent HeadTap() {
    pet::SenseEvent event;
    event.kind = pet::SenseKind::TouchTapFront;
    return event;
}
}  // namespace

int main() {
    lv_init();
    auto* disp = lv_display_create(320, 240);
    static uint8_t pixels[320 * 20 * 2];
    lv_display_set_buffers(disp, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, PointerRead);
    lv_timer_pause(lv_indev_get_read_timer(pointer));   // read by the test, once per step
    // The Owner allows the pet its own sounds and head moves outside a conversation.
    assert(CurrentOutputGate().Bind({true, 1,
        OutputBit(presentation::Output::Expression) | OutputBit(presentation::Output::AudioCue) |
        OutputBit(presentation::Output::Motion)}));
    std::vector<UiIntent> intents;
    SetEidolonUiIntentHandler([&](UiIntent intent) { intents.push_back(intent); });

    Display display;
    auto* screen = lv_obj_create(nullptr);
    lv_screen_load(screen);
    CompanionFaceView view;
    view.Build({screen, &font_puhui_basic_20_4, &display, &font_awesome_20_4});
    // Ready, as UiStateProjector draws it: the panel offers pick-up and setup.
    EidolonUiModel ready;
    ready.scene = UiScene::Ready;
    ready.touch_navigation = true;
    ready.primary_label = "JOIN";
    ready.primary_intent = UiIntent::OpenConversation;
    ready.primary_enabled = true;
    ready.primary_presentation = UiActionPresentation::TouchControl;
    ready.show_setup_action = true;
    view.Render(ready);
    Run(view, pointer, 200);

    // 1. A finger held on a button across the panel's 5 s idle close keeps the
    //    panel open, and the release still acts.
    Tap(pointer, {160, 10});            // the status bar opens the panel
    Run(view, pointer, 300);            // slide-in
    auto buttons = PanelButtons(screen);
    assert(buttons.size() == 2);        // pick up, setup
    auto* start = buttons[0];
    Run(view, pointer, 4400);           // about 4.7 s since it opened
    assert(lv_obj_is_visible(start));
    g_point = Centre(start);
    g_down = true;
    lv_indev_read(pointer);
    Run(view, pointer, 800);            // still held at 5.5 s
    assert(lv_obj_is_visible(start) && lv_obj_has_state(start, LV_STATE_PRESSED));
    assert(intents.empty());
    g_down = false;
    lv_indev_read(pointer);
    assert(intents.size() == 1 && intents[0] == UiIntent::OpenConversation);
    Run(view, pointer, 40);
    assert(!lv_obj_is_visible(start));  // a button acts, then the panel closes
    // With no finger on it the idle close still happens.
    Tap(pointer, {160, 10});
    Run(view, pointer, 300);
    assert(PanelButtons(screen).size() == 2);
    Run(view, pointer, 5000);
    assert(PanelButtons(screen).empty());

    // 2. An end drawn ahead of the runtime: the face is back at rest, but the
    //    microphone may still be open, so a tap on the head gets no chirp and no
    //    head move until the runtime confirms.
    EidolonUiModel ending = ready;      // what UiStateProjector draws for a pending end
    ending.scene = UiScene::Ended;
    ending.end_reason = EndReason::UserLeft;
    ending.primary_intent = UiIntent::None;
    ending.primary_enabled = false;
    ending.primary_label = "";
    ending.session_request_pending = true;
    view.Render(ending);
    Run(view, pointer, 100);
    const int chirps = pet::g_chirps;
    const int head_moves = Board::GetInstance().head_clips;
    view.PostSense(HeadTap());
    Run(view, pointer, 3000);
    assert(pet::g_chirps == chirps);
    assert(Board::GetInstance().head_clips == head_moves);
    // Confirmed: the same tap is answered.
    EidolonUiModel ended = ending;
    ended.session_request_pending = false;
    ended.primary_intent = UiIntent::OpenConversation;
    ended.primary_enabled = true;
    ended.primary_label = "JOIN";
    view.Render(ended);
    view.PostSense(HeadTap());
    Run(view, pointer, 100);
    assert(pet::g_chirps == chirps + 1);
    std::puts("pet panel ui: ok");
    return 0;
}
