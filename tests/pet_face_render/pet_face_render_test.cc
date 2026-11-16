// Renders the desk-pet face (views/pet_face_skin.*) driven by the real pet runtime
// on host LVGL, writes one PPM per state and checks that each state draws something
// recognisable (eyes lit, glyphs differ from open eyes, card/bubble present).
#include <chrono>
#include <lvgl.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "eidolon/pet/pet_runtime.h"
#include "eidolon/views/pet_face_skin.h"

LV_FONT_DECLARE(font_puhui_basic_20_4);
using namespace eidolon;
using namespace eidolon::pet;

namespace {
struct Shot { int w = 0, h = 0; std::string rgb; };

// What the panel shows: every flushed band lands here, exactly as the SPI flush
// would write it. Compared with a full redraw it exposes stale pixels left by a
// part box that is too small.
uint16_t g_panel[320 * 240];
lv_display_t* g_disp = nullptr;
int g_max_diff = 0;   // largest per-channel difference seen, 8-bit scale
uint64_t g_flushed_px = 0;   // pixels pushed to the panel so far (render cost proxy)

int StalePixels(lv_obj_t* screen) {
    lv_obj_update_layout(screen);
    auto* snap = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    assert(snap);
    int stale = 0, bx1 = 999, by1 = 999, bx2 = -1, by2 = -1, max_diff = 0;
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
        uint16_t v; std::memcpy(&v, snap->data + y * snap->header.stride + x * 2, 2);
        const uint16_t p = g_panel[y * 320 + x];
        const int dr = std::abs((v >> 11) - (p >> 11)), dg = std::abs(((v >> 5) & 63) - ((p >> 5) & 63)), db = std::abs((v & 31) - (p & 31));
        // Part signatures hash the exact primitives drawn, so the panel must match
        // a full redraw; allow only rounding noise between the two render paths.
        max_diff = std::max({max_diff, dr * 8, dg * 4, db * 8});
        if (dr > 1 || dg > 2 || db > 1) { ++stale; bx1 = std::min(bx1, x); by1 = std::min(by1, y); bx2 = std::max(bx2, x); by2 = std::max(by2, y); }
    }
    if (stale) std::printf("  stale box x %d..%d y %d..%d\n", bx1, bx2, by1, by2);
    g_max_diff = std::max(g_max_diff, max_diff);
    lv_draw_buf_destroy(snap);
    return stale;
}

Shot Take(lv_obj_t* screen) {
    lv_obj_update_layout(screen);
    auto* snap = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    assert(snap);
    Shot s; s.w = snap->header.w; s.h = snap->header.h; s.rgb.resize(s.w * s.h * 3);
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        const uint8_t* p = snap->data + y * snap->header.stride + x * 3;
        char* d = &s.rgb[(y * s.w + x) * 3]; d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
    }
    lv_draw_buf_destroy(snap);
    return s;
}
void Save(const Shot& s, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb"); assert(f);
    std::fprintf(f, "P6\n%d %d\n255\n", s.w, s.h);
    std::fwrite(s.rgb.data(), 1, s.rgb.size(), f); std::fclose(f);
}
int Luma(const Shot& s, int x, int y) {
    const auto* p = reinterpret_cast<const uint8_t*>(&s.rgb[(y * s.w + x) * 3]);
    return (p[0] * 3 + p[1] * 6 + p[2]) / 10;
}
// Fraction of bright pixels inside a box: the "ink" a state puts on screen there.
float Lit(const Shot& s, int x0, int y0, int x1, int y1, int threshold = 120) {
    int lit = 0, n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { ++n; if (Luma(s, x, y) > threshold) ++lit; }
    return n ? static_cast<float>(lit) / n : 0;
}

struct Rig {
    lv_obj_t* screen;
    PetFaceSkin skin;
    Runtime rt{7};
    RuntimeInputs in;
    uint64_t now = 1000;
    bool refresh = true;   // push every frame through the band renderer like the device
    bool probe = false;
    uint64_t redrawn_px = 0;     // pixels flushed while this rig ran
    uint32_t redraw_frames = 0;  // frames that flushed anything
    double redraw_us = 0;        // host time spent rendering those frames (relative cost only)
    explicit Rig(lv_obj_t* s) : screen(s) { skin.Build(screen, &font_puhui_basic_20_4); skin.SetCard("100% on power", "feeling calm"); }
    void Run(uint64_t ms, const std::function<void()>& each = {}) {
        for (uint64_t t = 0; t < ms; t += 20) {
            now += 20; lv_tick_inc(20); in.now_ms = now;
            if (each) each();
            skin.Show(rt.Tick(in), false);
            if (refresh) {
                const uint64_t before = g_flushed_px;
                const auto t0 = std::chrono::steady_clock::now();
                lv_refr_now(g_disp);
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                if (g_flushed_px > before) { redrawn_px += g_flushed_px - before; ++redraw_frames; redraw_us += us; }
            }
            // Every frame, not just the last: the panel must equal a full redraw.
            if (probe && refresh) assert(StalePixels(screen) == 0);
        }
    }
    void Sense(SenseKind k, float v = 0, int8_t dir = 0) { rt.OnSense({k, now, v, dir}); }
};
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string dir = argv[1];
    lv_init();
    auto* disp = lv_display_create(320, 240);
    g_disp = disp;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static uint8_t pixels[320 * 10 * 2];   // 10-line bands, like the StackChan panel
    lv_display_set_buffers(disp, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t* a, uint8_t* px) {
        const int w = a->x2 - a->x1 + 1;
        for (int y = a->y1; y <= a->y2; ++y)
            std::memcpy(&g_panel[y * 320 + a->x1], px + (y - a->y1) * w * 2, w * 2);
        g_flushed_px += static_cast<uint64_t>(w) * (a->y2 - a->y1 + 1);
        lv_display_flush_ready(d);
    });

    auto scene = [&](const char* name, const std::function<void(Rig&)>& drive) {
        auto* screen = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x121316), 0);
        lv_screen_load(screen);
        Rig r(screen);
        r.Run(2600);                       // settle the springs
        drive(r);
        if (!r.refresh) { r.refresh = true; r.Run(40); }
        lv_refr_now(g_disp);
        const int stale = StalePixels(screen);
        if (stale) std::printf("%s: %d stale pixels after incremental redraws\n", name, stale);
        assert(stale == 0);
        Shot s = Take(screen);
        Save(s, dir + "/" + name + ".ppm");
        lv_obj_delete(screen);
        return s;
    };

    // Open eyes: the eye boxes are lit, the background is dark.
    Shot calm = scene("01-calm", [](Rig& r) { r.Run(1); });
    assert(Lit(calm, 70, 95, 120, 150) > .5f && Lit(calm, 200, 95, 250, 150) > .5f);
    assert(Lit(calm, 0, 200, 40, 240) < .02f);
    // Antenna tips glow above the eyes.
    assert(Lit(calm, 40, 10, 110, 80, 60) > .01f && Lit(calm, 210, 10, 280, 80, 60) > .01f);

    Shot listen = scene("02-listening", [](Rig& r) { r.in.phase = Phase::Listening; r.in.mic_level = .85f; r.Run(1200); });
    Shot think = scene("03-thinking", [](Rig& r) { r.in.phase = Phase::Thinking; r.probe = true; r.Run(1500); });
    // Thinking lives on the antenna tips now: nothing beside the face (the old "...").
    assert(Lit(think, 282, 80, 318, 130, 80) < .002f);
    Shot delight = scene("04-host-delight", [](Rig& r) {
        r.in.host_active = true; r.in.host_gesture = 5; r.in.host_amount = 1; r.Run(500); });
    Shot fur = scene("05-stroke-with-fur", [](Rig& r) {
        r.Sense(SenseKind::TouchDown, -1); r.Sense(SenseKind::TouchStrokeStart, 0, +1); r.Run(900); });
    // A happy-arc glyph lights far less of the eye box than an open eye.
    assert(Lit(fur, 70, 95, 120, 150) < Lit(calm, 70, 95, 120, 150) * .7f);
    Shot shaken = scene("06-shaken", [](Rig& r) { r.Sense(SenseKind::Shaken); r.Run(900); });
    Shot grumpy = scene("07-rapid-taps", [](Rig& r) { r.Sense(SenseKind::TouchRapid); r.Run(700); });
    Shot picked = scene("08-picked-up", [](Rig& r) { r.Sense(SenseKind::PickedUp); r.Run(400); });
    Shot shy = scene("09-face-close", [](Rig& r) { r.Sense(SenseKind::FaceClose); r.Run(900); });
    Shot charger = scene("10-charger-in", [](Rig& r) { r.rt.OnPower(false, 60, r.now); r.rt.OnPower(true, 60, r.now); r.Run(900); });
    // Thought bubble above-right of the face.
    assert(Lit(charger, 190, 20, 260, 80, 80) > .01f);
    Shot card = scene("11-glance-card", [](Rig& r) { r.Sense(SenseKind::TouchDoubleTap); r.Run(900); });
    assert(Lit(card, 20, 175, 240, 230, 80) > .02f);   // card text and border
    assert(Lit(card, 250, 198, 314, 234, 40) < .002f);  // clear of the corner control
    Shot asleep = scene("12-asleep", [](Rig& r) { r.in.dark = true; r.refresh = false; r.Run(13ull * 60 * 1000); });
    Shot opening = scene("13-connecting", [](Rig& r) { r.in.phase = Phase::Opening; r.Run(700); });
    Shot muted = scene("14-muted", [](Rig& r) { r.in.phase = Phase::Listening; r.in.muted = true; r.Run(600); });
    Shot error = scene("15-error", [](Rig& r) { r.in.phase = Phase::Error; r.Run(610); });
    // Speaking: the face follows its own voice (syllables at ~3/s), checked pixel-exact every frame.
    // Report what each frame costs to redraw, the budget that decides smoothness while audio is busy.
    double speak_px = 0, listen_px = 0;
    Shot speaking = scene("16-speaking", [&](Rig& r) {
        r.in.phase = Phase::Responding; r.Run(400);
        r.redrawn_px = 0; r.redraw_frames = 0; r.probe = true;
        uint64_t t = 0;
        r.Run(6000, [&] { r.in.speech_dbfs = (t % 340) < 170 ? -18.f : -52.f; t += 20; });
        speak_px = r.redraw_frames ? static_cast<double>(r.redrawn_px) / r.redraw_frames : 0;
        std::printf("speaking: %u of 300 frames redrew, %.0f px per frame (%.1f%% of the screen), host %.0f us per frame\n",
                    r.redraw_frames, speak_px, 100.0 * speak_px / (320 * 240), r.redraw_frames ? r.redraw_us / r.redraw_frames : 0);
    });
    scene("17-listening-cost", [&](Rig& r) {
        r.in.phase = Phase::Listening; r.Run(400);
        r.redrawn_px = 0; r.redraw_frames = 0;
        uint64_t t = 0;
        r.Run(6000, [&] { r.in.mic_level = (t % 340) < 170 ? .7f : .1f; t += 20; });
        listen_px = r.redraw_frames ? static_cast<double>(r.redrawn_px) / r.redraw_frames : 0;
        std::printf("listening: %u of 300 frames redrew, %.0f px per frame (%.1f%% of the screen)\n",
                    r.redraw_frames, listen_px, 100.0 * listen_px / (320 * 240));
    });
    // Each sounding reaction at the moment its chirp plays, for the sound-vs-face audition
    // sheet (chirp-<name>.ppm); pixel-exact like every other scene.
    struct Moment { const char* name; std::function<void(Rig&)> start; uint64_t at_ms; };
    const Moment moments[] = {
        {"chirp-bip", [](Rig& r) { r.Sense(SenseKind::TouchTapFront); }, 300},
        {"chirp-boing", [](Rig& r) { r.Sense(SenseKind::TouchTapMiddle); }, 200},
        {"chirp-huh", [](Rig& r) { r.Sense(SenseKind::TouchTapBack); }, 500},
        {"chirp-purr", [](Rig& r) { r.Sense(SenseKind::TouchDown, -1); r.Sense(SenseKind::TouchStrokeStart, 0, +1); }, 700},
        {"chirp-grumble", [](Rig& r) { r.Sense(SenseKind::TouchDown, 1); r.Sense(SenseKind::TouchStrokeStart, 0, -1); }, 600},
        {"chirp-hmph", [](Rig& r) { r.Sense(SenseKind::TouchRapid); }, 700},
        {"chirp-pop", [](Rig& r) { r.Sense(SenseKind::TouchDoubleTap); }, 900},
        {"chirp-eep", [](Rig& r) { r.Sense(SenseKind::PickedUp); }, 400},
        {"chirp-phew", [](Rig& r) { r.Sense(SenseKind::PutDown); }, 500},
        {"chirp-dizzy", [](Rig& r) { r.Sense(SenseKind::Shaken); }, 900},
        {"chirp-knock", [](Rig& r) { r.Sense(SenseKind::Knock); }, 400},
        {"chirp-hmm", [](Rig& r) { r.Sense(SenseKind::Covered); }, 800},
        {"chirp-peekaboo", [](Rig& r) { r.Sense(SenseKind::Uncovered); }, 400},
        {"chirp-yawn", [](Rig& r) { r.Sense(SenseKind::LightsOff); }, 900},
        {"chirp-wake", [](Rig& r) { r.Sense(SenseKind::LightsOn); }, 500},
        {"chirp-giggle", [](Rig& r) { r.Sense(SenseKind::FaceClose); }, 900},
        {"chirp-yum", [](Rig& r) { r.rt.OnPower(false, 60, r.now); r.rt.OnPower(true, 60, r.now); }, 900},
        {"chirp-sad", [](Rig& r) { r.Sense(SenseKind::BatteryLow); }, 900},
    };
    for (const Moment& m : moments) scene(m.name, [&m](Rig& r) { m.start(r); r.Run(m.at_ms); });
    // The status bar owns y < 32 (session/action icons, battery, network, gear): no
    // face ink may reach it in any state, the glance-card lift included.
    const Shot* all[] = {&calm, &listen, &think, &delight, &fur, &shaken, &grumpy, &picked,
                         &shy, &charger, &card, &asleep, &opening, &muted, &error, &speaking};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const float band = Lit(*all[i], 0, 0, 320, 32, 60);
        if (band >= .001f) std::printf("status bar band lit %.4f in shot %zu\n", band, i + 1);
        assert(band < .001f);
    }
    // The corner JOIN / END control sits at (250..314, 198..234): free in the calm pose.
    assert(Lit(calm, 250, 198, 314, 234, 60) < .01f);
    std::printf("largest panel-vs-redraw difference: %d/255\n", g_max_diff);
    std::puts("pet face render tests passed");
    return 0;
}
