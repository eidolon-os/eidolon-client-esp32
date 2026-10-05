#include <lvgl.h>
#include <font_awesome.h>
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "display.h"
#include "cbin_font_fixture.h"
#include "eidolon/views/companion_face_view.h"
#include "eidolon/views/companion_layout.h"
#include "eidolon/output_policy.h"
LV_FONT_DECLARE(font_noto_basic_16_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_puhui_basic_20_4);
using namespace eidolon;
static lv_obj_t* Find(lv_obj_t* obj,const char* text) {
    if (lv_obj_check_type(obj,&lv_label_class) && !std::strcmp(lv_label_get_text(obj),text)) return obj;
    for (unsigned i=0;i<lv_obj_get_child_count(obj);++i)
        if (auto* found=Find(lv_obj_get_child(obj,i),text)) return found;
    return nullptr;
}
static void Within(lv_obj_t* obj,companion::Rect r) {
    lv_obj_update_layout(obj);lv_area_t a;lv_obj_get_coords(obj,&a);
    if (!(a.x1>=r.x && a.y1>=r.y && a.x2<r.x+r.w && a.y2<r.y+r.h)) std::fprintf(stderr,"bounds: (%d,%d)-(%d,%d), expected (%d,%d,%d,%d)\n",a.x1,a.y1,a.x2,a.y2,r.x,r.y,r.w,r.h);
    assert(a.x1>=r.x && a.y1>=r.y && a.x2<r.x+r.w && a.y2<r.y+r.h);
}
static void Screenshot(lv_obj_t* screen,const std::string& path) {
    lv_obj_update_layout(screen);
    auto* shot=lv_snapshot_take(screen,LV_COLOR_FORMAT_RGB888);assert(shot);
    FILE* file=std::fopen(path.c_str(),"wb");assert(file);
    std::fprintf(file,"P6\n%u %u\n255\n",shot->header.w,shot->header.h);
    for (unsigned y=0;y<shot->header.h;++y) for(unsigned x=0;x<shot->header.w;++x) {
        auto* p=shot->data+y*shot->header.stride+x*3;
        const unsigned char rgb[]={p[2],p[1],p[0]};std::fwrite(rgb,1,3,file);
    }
    std::fclose(file);lv_draw_buf_destroy(shot);
}
static lv_point_t pointer_point;
static bool pointer_down;
static void PointerRead(lv_indev_t*, lv_indev_data_t* data) {
    data->point = pointer_point;
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void ClickAt(lv_indev_t* input, int x, int y) {
    pointer_point = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
    pointer_down = true; lv_indev_read(input);
    pointer_down = false; lv_indev_read(input);
}
int main(int argc,char** argv) {
    assert(argc==2);lv_init();
    auto* disp=lv_display_create(320,240);
    static uint8_t pixels[320*20*2];
    lv_display_set_buffers(disp,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp,[](lv_display_t* d,const lv_area_t*,uint8_t*){lv_display_flush_ready(d);});
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, PointerRead);
    Display display;
    const companion::Layout layout(320,240);
    auto& gate=CurrentOutputGate();
    const auto expression=OutputBit(presentation::Output::Expression);
    const auto dialogue=OutputBit(presentation::Output::DialogueText);
    const auto speech=OutputBit(presentation::Output::Speech);
    assert(gate.Bind({true,1,expression|dialogue|speech}));
    std::vector<UiIntent> events;SetEidolonUiIntentHandler([&](UiIntent i){events.push_back(i);});
    for (const auto* font : {&font_noto_basic_16_4,&font_puhui_basic_20_4}) {
        auto* screen=lv_obj_create(nullptr);lv_screen_load(screen);
        CompanionFaceView view;
        view.SetSystemStatus("Before setup");view.ShowNotification("Before setup",1000);
        view.Build({screen,font,&display,&font_awesome_20_4});
        lv_label_set_text(view.indicators().network,FONT_AWESOME_WIFI);
        lv_label_set_text(view.indicators().battery,FONT_AWESOME_BATTERY_FULL);
        EidolonUiModel model;model.touch_navigation=true;model.scene=UiScene::Conversation;model.status_text="Listening";
        model.mode_label="FULL";model.primary_label="MIC";model.primary_enabled=true;
        model.primary_intent=UiIntent::ToggleMicrophone;model.show_end_action=true;
        model.primary_presentation=UiActionPresentation::TouchControl;
        model.subtitle="PRIVATE DIALOGUE MUST NOT LEAK";
        view.SetMotionDiagnostic("nod","rejected","BUSY");
        assert(Find(screen,"F:\nM:nod:rejected/BUSY"));
        assert(gate.Start({"silent",1,expression,true},"silent"));
        for(auto mode:{InteractionMode::FullDuplex,InteractionMode::HalfDuplex,InteractionMode::PushToTalk}) {
            model.interaction_mode=mode;
            // Every useful output combination remains independent of input mode.
            for (uint32_t selected=1;selected<8;++selected) {
                uint32_t mask=(selected&1 ? expression : 0)|(selected&2 ? dialogue : 0)|(selected&4 ? speech : 0);
                assert(gate.Start({"matrix",1,mask,bool(mask&expression)},"matrix"));
                view.Render(model);
                assert(bool(Find(screen,model.subtitle))==bool(mask&dialogue));
                assert(gate.Allows(presentation::Output::Speech)==bool(mask&speech));
                assert(gate.Allows(presentation::Output::Expression)==bool(mask&expression));
            }
            assert(gate.Start({"silent",1,expression,true},"silent"));
            view.Render(model);assert(!Find(screen,model.subtitle));
        }
        view.SetSystemStatus("Obsolete raw state");assert(Find(screen,"Listening"));
        assert(!Find(screen,"Obsolete raw state"));
        view.Advance(0);
        Within(Find(screen,"Listening"),layout.header);
        Within(Find(screen,"MIC"),layout.actions);
        Within(Find(screen,"END"),layout.actions);
        const std::string prefix=std::string(argv[1])+(font==&font_noto_basic_16_4 ? "/box3" : "/stackchan");
        Screenshot(screen,prefix+"-silent.ppm");
        expression::Plan diagnostic_plan;diagnostic_plan.token=765;
        diagnostic_plan.count=1;diagnostic_plan.max_duration_ms=1000;
        diagnostic_plan.steps[0]={expression::Gesture::Delight,.5f,0,1000,false};
        assert(view.Submit(diagnostic_plan).status==expression::Status::Accepted);
        view.SetMotionDiagnostic("wake_wobble","started","");
        auto* diagnostic=Find(screen,"F:delight:accepted\nM:wake_wobble:started");
        assert(diagnostic && !lv_obj_has_flag(diagnostic,LV_OBJ_FLAG_HIDDEN));
        Within(diagnostic,{8,36,304,160});
        Screenshot(screen,prefix+"-diagnostic.ppm");
        view.Cancel(765);
        lv_tick_inc(5100);view.Advance(0);
        assert(lv_obj_has_flag(diagnostic,LV_OBJ_FLAG_HIDDEN));
        auto* mic=lv_obj_get_parent(Find(screen,"MIC"));
        lv_area_t mic_area; lv_obj_get_coords(mic, &mic_area);
        const auto before_click = events.size();
        ClickAt(pointer, (mic_area.x1+mic_area.x2)/2, (mic_area.y1+mic_area.y2)/2);
        assert(events.size()==before_click+1 && events.back()==UiIntent::ToggleMicrophone);
        ClickAt(pointer, 160, 100);
        assert(events.size()==before_click+1); // touching the face is not a session toggle
        model.primary_presentation=UiActionPresentation::InputHint;
        model.input_hint="Press button to end";model.show_end_action=false;
        view.Render(model);
        assert(!lv_obj_is_visible(mic));
        assert(!lv_obj_is_visible(lv_obj_get_parent(Find(screen,"END"))));
        assert(lv_obj_is_visible(Find(screen,model.input_hint)));
        ClickAt(pointer, (mic_area.x1+mic_area.x2)/2, (mic_area.y1+mic_area.y2)/2);
        assert(events.size()==before_click+1);
        Screenshot(screen,prefix+"-physical-input.ppm");
        model.scene=UiScene::Ready;model.status_text="Ready";model.detail_text="Start a conversation";
        model.input_hint="Press button to start";
        // Real BOX-3 profile has no battery. Keep the touch-board fixture intact.
        if (font==&font_noto_basic_16_4) lv_label_set_text(view.indicators().battery,"");
        view.Render(model);
        assert(!Find(screen,model.detail_text));
        auto* ready_hint=Find(screen,model.input_hint);
        assert(ready_hint && lv_obj_is_visible(ready_hint));
        Within(ready_hint,layout.actions);
        lv_area_t hint_area;lv_obj_get_coords(ready_hint,&hint_area);
        assert(hint_area.y2<230); // at least ten pixels of bottom breathing room
        Screenshot(screen,prefix+"-ready-physical.ppm");
        model.scene=UiScene::Conversation;model.status_text="Listening";
        model.primary_presentation=UiActionPresentation::TouchControl;model.show_end_action=true;
        model.mode_label="PTT";model.primary_label="TALK";model.primary_intent=UiIntent::BeginTalk;
        view.Render(model);auto* talk=lv_obj_get_parent(Find(screen,"TALK"));
        const auto count=events.size();lv_obj_send_event(talk,LV_EVENT_PRESSED,nullptr);
        model.primary_label="REC";model.primary_intent=UiIntent::CommitTalk;model.turn=TurnPhase::Recording;
        view.Render(model);lv_obj_send_event(talk,LV_EVENT_RELEASED,nullptr);
        lv_obj_send_event(talk,LV_EVENT_CLICKED,nullptr);
        assert(events.size()==count+2 && events[count]==UiIntent::BeginTalk && events[count+1]==UiIntent::CommitTalk);
        Screenshot(screen,prefix+"-ptt.ppm");
        assert(gate.Start({"text",1,expression|dialogue,true},"text"));
        model.interaction_mode=InteractionMode::FullDuplex;model.mode_label="FULL";
        model.primary_label="MIC";model.primary_intent=UiIntent::ToggleMicrophone;model.turn=TurnPhase::Idle;
        model.subtitle="This is a long response that scrolls only below the face, while the controls remain visible.";
        lv_tick_inc(250);
        view.Render(model);
        auto* captions=lv_obj_get_parent(Find(screen,model.subtitle));
        const companion::Layout caption_layout(320,240,true,true,std::max<int>(52,2*font->line_height+4));
        Within(captions,caption_layout.information);
        lv_obj_update_layout(captions);
        assert(lv_obj_get_scroll_bottom(captions)>0);
        lv_tick_inc(3500);view.Advance(0);
        const int page_y=lv_obj_get_scroll_y(captions);
        assert(page_y>0);
        view.Render(model);assert(lv_obj_get_scroll_y(captions)==page_y);
        // Fast revisions coalesce, then flush on the view timer.
        model.subtitle="First revision";
        view.Render(model);assert(Find(screen,model.subtitle));
        model.subtitle="Latest revision";
        view.Render(model);assert(!Find(screen,model.subtitle));
        lv_tick_inc(250);view.Advance(0);assert(Find(screen,model.subtitle));
        // Empty captions never move the face while dialogue output is enabled.
        const auto reserved=companion::Layout(320,240,true,true,std::max<int>(52,2*font->line_height+4));
        model.subtitle="";view.Render(model);
        assert(lv_obj_is_visible(captions));Within(captions,reserved.information);
        model.subtitle="This is a long response. Captions stay below the face and above the controls.";
        view.Render(model);
        Screenshot(screen,prefix+"-text.ppm");
        // BOX-3's shipped profile uses its physical button and has no battery.
        model.primary_presentation=UiActionPresentation::InputHint;
        model.input_hint="Press button to end";model.show_end_action=false;
        lv_label_set_text(view.indicators().battery,"");
        view.Render(model);
        Screenshot(screen,prefix+"-voice-physical.ppm");
        assert(!lv_obj_is_visible(view.indicators().battery));
        model.primary_presentation=UiActionPresentation::TouchControl;
        model.show_end_action=true;
        lv_label_set_text(view.indicators().battery,FONT_AWESOME_BATTERY_FULL);
        view.Render(model);
        view.ShowNotification("Volume 60%",1000);assert(Find(screen,"Volume 60%"));
        assert(Find(screen,model.subtitle)); // notifications never replace speech
        model.show_mute_icon=true;model.show_setup_action=true;
        view.Render(model);
        auto* mute=Find(screen,FONT_AWESOME_MICROPHONE_SLASH);assert(mute);
        Within(mute,layout.header);
        auto* setup=Find(screen,FONT_AWESOME_GEAR);assert(setup);
        Within(setup,layout.header);
        lv_font_glyph_dsc_t glyph;
        for (uint32_t cp : {0xf013u,0xf131u,0xf1ebu,0xf240u})
            assert(lv_font_get_glyph_dsc(&font_awesome_20_4,&glyph,cp,0));
        auto* notice=Find(screen,"Volume 60%");
        lv_obj_update_layout(notice);
        lv_area_t notice_bounds;lv_obj_get_coords(notice,&notice_bounds);
        assert(notice_bounds.x1==12 && notice_bounds.x2==307);
        Screenshot(screen,prefix+"-icons-notification.ppm");
        model.show_mute_icon=false;view.Render(model);
        assert(!lv_obj_is_visible(mute));
        model.show_setup_action=false;
        view.Render(model);assert(Find(screen,"Volume 60%"));
        lv_tick_inc(1100);view.Advance(0);assert(Find(screen,model.subtitle));
        // A short subtitle expires after reading, and duplicate models cannot revive it.
        model.subtitle="Finished speaking.";lv_tick_inc(250);view.Render(model);
        lv_tick_inc(6100);view.Advance(0);assert(!Find(screen,model.subtitle));
        view.Render(model);assert(!Find(screen,model.subtitle));
        model.scene=UiScene::RecoveryRequired;model.status_text="Connection lost";
        model.detail_text="Please check your network.\nReconnect the device to Wi-Fi and retry.\nLong setup instructions stay on this page.\nSwipe vertically to read more.\nThe action row stays fixed.";
        model.severity=UiSeverity::Error;model.primary_enabled=false;model.show_end_action=false;
        view.Render(model);auto* detail=Find(screen,model.detail_text);assert(detail);
        auto* panel=lv_obj_get_parent(detail);Within(panel,layout.detail);
        lv_obj_update_layout(panel);assert(lv_obj_get_scroll_bottom(panel)>0);
        assert(Find(screen,"Swipe to read more"));
        assert(!lv_obj_is_visible(talk));
        Screenshot(screen,prefix+"-recovery.ppm");
        lv_obj_scroll_to_y(panel,40,LV_ANIM_OFF);view.Render(model);
        assert(lv_obj_get_scroll_y(panel)==40); // periodic model updates must not reset reading
        model.touch_navigation=false;view.Render(model);
        assert(!Find(screen,"Swipe to read more"));
        lv_obj_scroll_to_y(panel,0,LV_ANIM_OFF);
        lv_tick_inc(3000);view.Advance(0);
        assert(lv_obj_get_scroll_y(panel)>0); // no-touch devices can read overflowing recovery text
        // Declaring text output off clears dialogue in every interaction mode.
        gate.Close();model.scene=UiScene::Conversation;view.Render(model);assert(!Find(screen,model.subtitle));

    }
    {
        CbinFontFixture resource(CBIN_FONT_FILE);
        auto* screen=lv_obj_create(nullptr);lv_screen_load(screen);
        CompanionFaceView view;view.Build({screen,&font_noto_basic_16_4,&display,&font_awesome_20_4});
        assert(gate.Start({"chinese",1,expression|dialogue,true},"chinese"));
        EidolonUiModel model;model.scene=UiScene::Conversation;model.status_text="Speaking";
        lv_label_set_text(view.indicators().network,FONT_AWESOME_WIFI);
        model.primary_presentation=UiActionPresentation::InputHint;
        model.input_hint="Press button to end";
        model.turn=TurnPhase::AgentSpeaking;model.subtitle="我叫小何。";
        view.Render(model);
        lv_font_glyph_dsc_t glyph;
        assert(!lv_font_get_glyph_dsc(&font_noto_basic_16_4,&glyph,U'叫',0) || glyph.is_placeholder);
        Screenshot(screen,std::string(argv[1])+"/box3-chinese-before.ppm");
        view.SetContentFont(resource.font());view.Advance(0);
        auto* caption=Find(screen,model.subtitle);assert(caption);
        assert(lv_obj_get_style_text_font(caption,LV_PART_MAIN)==resource.font());
        for (uint32_t cp : {U'我',U'叫',U'小',U'何',U'。'}) {
            assert(lv_font_get_glyph_dsc(resource.font(),&glyph,cp,0));
            assert(!glyph.is_placeholder && glyph.box_w>0);
        }
        const companion::Layout chinese_layout(320,240,true,true,std::max<int>(52,2*resource.font()->line_height+4));
        auto* captions=lv_obj_get_parent(caption);
        Within(captions,chinese_layout.information);
        assert(lv_obj_get_scroll_bottom(captions)==0);
        Screenshot(screen,std::string(argv[1])+"/box3-chinese-after.ppm");
        model.subtitle="我叫小何。我叫小何。我叫小何。我叫小何。我叫小何。我叫小何。我叫小何。我叫小何。我叫小何。";
        model.turn=TurnPhase::Idle;
        lv_tick_inc(250);view.Render(model);
        lv_obj_update_layout(captions);assert(lv_obj_get_scroll_bottom(captions)>0);
        lv_tick_inc(3500);view.Advance(0);
        assert(lv_obj_get_scroll_y(captions)==2*(resource.font()->line_height+4));
        Screenshot(screen,std::string(argv[1])+"/box3-chinese-page.ppm");
        // Stream one character at a time: show partial text before stream close,
        // fill the full 304px strip, and follow the latest spoken line.
        model.subtitle="";view.Render(model);model.turn=TurnPhase::AgentSpeaking;
        const std::string streaming="我叫小何，今天我们一起测试语音字幕是否能够随着说话逐步显示，并且在屏幕右侧正常换行。文字变小后可以展示更多内容，中英文都保持清晰。";
        std::string partial;
        for (size_t i=0;i<streaming.size();i+=3) {
            partial.append(streaming,i,3);model.subtitle=partial.c_str();
            lv_tick_inc(250);view.Render(model);
            auto* visible=Find(screen,partial.c_str());assert(visible);
            assert(lv_obj_get_width(visible)==304);
            assert(lv_obj_get_style_text_align(visible,LV_PART_MAIN)==LV_TEXT_ALIGN_LEFT);
        }
        assert(lv_obj_get_scroll_y(captions)>0);
        assert(lv_obj_get_scroll_bottom(captions)==0);
        const int stream_y=lv_obj_get_scroll_y(captions);
        lv_tick_inc(4000);view.Advance(0);
        assert(lv_obj_get_scroll_y(captions)==stream_y);
        Screenshot(screen,std::string(argv[1])+"/box3-streaming-chinese.ppm");
        model.subtitle="我叫小何，很高兴见到你。\n今天想聊点什么？";
        lv_tick_inc(250);view.Render(model);
        Screenshot(screen,std::string(argv[1])+"/box3-design-conversation.ppm");
        model.subtitle="Streaming captions now follow speech. Smaller text uses the full screen width with balanced margins.";
        lv_tick_inc(250);view.Render(model);
        Screenshot(screen,std::string(argv[1])+"/box3-streaming-english.ppm");
        // Unloading/reloading must rebind labels before the mapped font disappears.
        view.SetContentFont(&font_noto_basic_16_4);
        assert(lv_obj_get_style_text_font(Find(screen,model.subtitle),LV_PART_MAIN)==&font_noto_basic_16_4);
        view.SetContentFont(resource.font());
        gate.Close();view.Render(model);assert(!Find(screen,model.subtitle));
        view.SetContentFont(&font_noto_basic_16_4);
    }
    // Xiaoling: the same view lives in an inscribed square on a round panel.
    // Exercise non-touch status/chrome and conversation at the actual safe size.
    lv_display_set_resolution(disp,360,360);
    {
        auto* screen=lv_obj_create(nullptr);lv_screen_load(screen);
        auto* viewport=lv_obj_create(screen);lv_obj_remove_style_all(viewport);
        lv_obj_set_size(viewport,252,252);lv_obj_center(viewport);
        CompanionFaceView view;
        view.Build({viewport,&font_puhui_basic_20_4,&display,&font_awesome_20_4});
        assert(gate.Bind({true,99,expression|dialogue|speech}));
        EidolonUiModel model;model.scene=UiScene::Conversation;
        model.status_text="Listening";model.mode_label="HALF";
        model.touch_navigation=false;model.turn=TurnPhase::UserSpeaking;
        model.primary_presentation=UiActionPresentation::Hidden;
        model.subtitle="Hello Xiaoling";
        view.Render(model);lv_obj_update_layout(screen);
        const companion::Rect safe{54,54,252,252};
        Within(viewport,safe);
        Within(view.indicators().network,safe);
        Within(view.indicators().battery,safe);
        auto* status=Find(viewport,"Listening");assert(status);Within(status,safe);
        // The farthest safe-square corners must remain inside the physical disk.
        assert(126*126*2 <= 180*180);
        Screenshot(screen,std::string(argv[1])+"/xiaoling-round-conversation.ppm");
    }
    lv_deinit();
    std::puts("Companion UI: both board fonts, layout bounds, scroll, notifications, silent modes and PTT events passed");
}
