#include <lvgl.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "display.h"
#include "eidolon/views/companion_face_view.h"
#include "eidolon/views/companion_layout.h"
#include "eidolon/output_policy.h"
LV_FONT_DECLARE(font_noto_basic_20_4);
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
int main(int argc,char** argv) {
    assert(argc==2);lv_init();
    auto* disp=lv_display_create(320,240);
    static uint8_t pixels[320*20*2];
    lv_display_set_buffers(disp,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp,[](lv_display_t* d,const lv_area_t*,uint8_t*){lv_display_flush_ready(d);});
    Display display;
    const companion::Layout layout(320,240);
    auto& gate=CurrentOutputGate();
    const auto expression=OutputBit(presentation::Output::Expression);
    const auto dialogue=OutputBit(presentation::Output::DialogueText);
    const auto speech=OutputBit(presentation::Output::Speech);
    assert(gate.Bind({true,1,expression|dialogue|speech}));
    std::vector<UiIntent> events;SetEidolonUiIntentHandler([&](UiIntent i){events.push_back(i);});
    for (const auto* font : {&font_noto_basic_20_4,&font_puhui_basic_20_4}) {
        auto* screen=lv_obj_create(nullptr);lv_screen_load(screen);
        CompanionFaceView view;
        view.SetSystemStatus("Before setup");view.ShowNotification("Before setup",1000);
        view.Build({screen,font,&display,&font_awesome_20_4});
        lv_label_set_text(view.indicators().network,LV_SYMBOL_WIFI);
        lv_label_set_text(view.indicators().battery,LV_SYMBOL_BATTERY_FULL);
        EidolonUiModel model;model.scene=UiScene::Conversation;model.status_text="Listening";
        model.mode_label="FULL";model.primary_label="MIC";model.primary_enabled=true;
        model.primary_intent=UiIntent::ToggleMicrophone;model.show_end_action=true;
        model.subtitle="PRIVATE DIALOGUE MUST NOT LEAK";
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
        const std::string prefix=std::string(argv[1])+(font==&font_noto_basic_20_4 ? "/box3" : "/stackchan");
        Screenshot(screen,prefix+"-silent.ppm");
        auto* mic=lv_obj_get_parent(Find(screen,"MIC"));
        lv_obj_send_event(mic,LV_EVENT_CLICKED,nullptr);assert(events.back()==UiIntent::ToggleMicrophone);
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
        view.Render(model);Within(Find(screen,model.subtitle),layout.information);
        Screenshot(screen,prefix+"-text.ppm");
        view.ShowNotification("Volume 60%",1000);assert(Find(screen,"Volume 60%"));
        view.Render(model);assert(Find(screen,"Volume 60%"));
        lv_tick_inc(1100);view.Advance(0);assert(Find(screen,model.subtitle));
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
        // Declaring text output off clears dialogue in every interaction mode.
        gate.Close();model.scene=UiScene::Conversation;view.Render(model);assert(!Find(screen,model.subtitle));

    }
    lv_deinit();
    std::puts("Companion UI: both board fonts, layout bounds, scroll, notifications, silent modes and PTT events passed");
}
