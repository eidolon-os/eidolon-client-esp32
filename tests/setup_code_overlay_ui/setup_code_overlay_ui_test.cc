#include <lvgl.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "eidolon/setup_code_card.h"
#include "setup_code_overlay.h"

namespace {

namespace df = eidolon::device_foundation::v1;

lv_obj_t* FindLabel(lv_obj_t* obj, const char* text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && std::strcmp(lv_label_get_text(obj), text) == 0) {
        return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (auto* found = FindLabel(lv_obj_get_child(obj, i), text)) return found;
    }
    return nullptr;
}

lv_obj_t* FindOfClass(lv_obj_t* obj, const lv_obj_class_t* cls)
{
    if (lv_obj_check_type(obj, cls)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (auto* found = FindOfClass(lv_obj_get_child(obj, i), cls)) return found;
    }
    return nullptr;
}

lv_area_t Coords(lv_obj_t* obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

bool Overlap(const lv_area_t& a, const lv_area_t& b)
{
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

void Screenshot(lv_obj_t* layer, const std::string& path)
{
    lv_obj_update_layout(layer);
    auto* shot = lv_snapshot_take(layer, LV_COLOR_FORMAT_RGB888);
    assert(shot);
    FILE* file = std::fopen(path.c_str(), "wb");
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

// How many pixels one module of the QR code is, read off the rendered screen.
// The first dark row inside the code is the top edge of a finder pattern:
// seven modules long, after a quiet zone of four. Zero if it is not that.
int32_t ModulePixels(lv_obj_t* layer, const lv_area_t& qr)
{
    lv_obj_update_layout(layer);
    lv_draw_buf_t* shot = lv_snapshot_take(layer, LV_COLOR_FORMAT_RGB888);
    assert(shot);
    auto dark = [shot](int32_t x, int32_t y) {
        return shot->data[y * shot->header.stride + x * 3] < 0x80;
    };
    int32_t module = 0;
    for (int32_t y = qr.y1; y <= qr.y2 && module == 0; ++y) {
        int32_t x = qr.x1;
        while (x <= qr.x2 && !dark(x, y)) ++x;
        if (x > qr.x2) continue;
        int32_t run = 0;
        while (x + run <= qr.x2 && dark(x + run, y)) ++run;
        const int32_t m = run / 7;
        module = (run % 7 == 0 && x - qr.x1 == 4 * m && y - qr.y1 == 4 * m) ? m : -1;
    }
    lv_draw_buf_destroy(shot);
    return module > 0 ? module : 0;
}

lv_display_t* MakeDisplay(int32_t width, int32_t height)
{
    lv_display_t* display = lv_display_create(width, height);
    static uint8_t pixels[800 * 20 * 2];
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
        lv_display_flush_ready(d);
    });
    lv_display_set_default(display);
    return display;
}

struct Screen {
    const char* board;
    int32_t width;
    int32_t height;
    // What it advertises over SoftAP, the default transport:
    // "eidolon-<board type, esp32-s3- dropped, at most 18 characters>-<MAC tail>".
    const char* name;
};

// Every maintained board with a screen, at the resolution LVGL is given.
// StackChan is the one that showed the bug: its 32-character name widened the
// column under the code until the digits beside it ran off the right edge.
constexpr Screen kScreens[] = {
    {"esp-box-3", 320, 240, "eidolon-esp-box-3-d84a04"},
    {"m5stack-core-s3", 320, 240, "eidolon-m5stack-core-s3-d84a04"},
    {"m5stack-stackchan", 320, 240, "eidolon-m5stack-stackchan-d84a04"},
    {"atk-dnesp32s3", 320, 240, "eidolon-atk-dnesp32s3-d84a04"},
    {"xiaoling", 360, 360, "eidolon-xiaoling-d84a04"},
    {"esp32-s3-touch-amoled-2.06", 410, 502, "eidolon-touch-amoled-2.06-d84a04"},
    {"korvo-1", 800, 480, "eidolon-korvo-1-d84a04"},
};

struct Card {
    std::string label;
    std::string name;
    // A name a maintained board really advertises: the code keeps a size a
    // phone reads from arm's length. The made-up worst cases need only fit.
    bool real;
};

// Every name a screen may have to carry: each maintained board's, since a
// card is laid out by the screen it lands on and not by the board it names;
// the BLE form; and two that no board has but the layout must still survive:
// the longest SoftAP name in wide glyphs, and one with nowhere to break a line.
std::vector<Card> Cards()
{
    std::vector<Card> cards;
    for (const Screen& screen : kScreens) cards.push_back({screen.board, screen.name, true});
    cards.push_back({"ble", eidolon::BleSetupName("m5stack-stackchan", "68:ee:8f:d8:4a:04"), true});
    cards.push_back({"longest", "eidolon-mwmwmwmwmwmwmwmwmw-dddddd", false});
    cards.push_back({"unbroken", std::string(33, 'W'), false});
    return cards;
}

struct Part {
    const char* what;
    lv_obj_t* obj;
};

// Show one card on one screen and check it; the case's own name in failures
// and, for a failure, a screenshot of what a person would have seen.
void CheckCard(const Screen& size, const Card& named, bool touch, bool with_notice,
               const std::string& out_dir)
{
    const std::string id = std::string(size.board) + "-" + named.label +
                            (touch ? "-touch" : "") + (with_notice ? "-notice" : "");
    const auto card =
        eidolon::MakeSetupCodeCard("40718256", named.name, df::SetupQrTransport::SoftAp);
    SetupCodeScreen screen{card.qr_json, card.code, card.advertised_name,
                           with_notice ? "A new code was drawn" : "", nullptr};
    if (touch) screen.cancel = [] {};

    lv_display_t* display = MakeDisplay(size.width, size.height);
    SetupCodeOverlay overlay;
    overlay.Show(display, screen);
    if (!overlay.visible()) std::fprintf(stderr, "%s: not shown\n", id.c_str());
    assert(overlay.visible());
    lv_obj_t* layer = lv_display_get_layer_top(display);
    lv_obj_update_layout(layer);

    std::vector<Part> parts = {
        {"qr", FindOfClass(layer, &lv_canvas_class)},
        {"name", FindLabel(layer, named.name.c_str())},
        {"hint", FindLabel(layer, "Scan with the Eidolon app, or enter")},
        {"first half", FindLabel(layer, "4071")},
        {"second half", FindLabel(layer, "8256")},
    };
    if (with_notice) parts.push_back({"notice", FindLabel(layer, "A new code was drawn")});
    if (touch) parts.push_back({"cancel", FindOfClass(layer, &lv_button_class)});
    if (!touch) assert(FindOfClass(layer, &lv_button_class) == nullptr);

    bool ok = true;
    for (const Part& part : parts) {
        if (part.obj == nullptr) {
            std::fprintf(stderr, "%s: no %s\n", id.c_str(), part.what);
            ok = false;
            continue;
        }
        const lv_area_t a = Coords(part.obj);
        if (!(a.x1 >= 0 && a.y1 >= 0 && a.x2 < size.width && a.y2 < size.height)) {
            std::fprintf(stderr, "%s: %s off the %dx%d screen at (%d,%d)-(%d,%d)\n", id.c_str(),
                         part.what, static_cast<int>(size.width), static_cast<int>(size.height),
                         static_cast<int>(a.x1), static_cast<int>(a.y1), static_cast<int>(a.x2),
                         static_cast<int>(a.y2));
            ok = false;
        }
    }
    for (size_t i = 0; ok && i < parts.size(); ++i) {
        for (size_t j = i + 1; j < parts.size(); ++j) {
            if (Overlap(Coords(parts[i].obj), Coords(parts[j].obj))) {
                std::fprintf(stderr, "%s: %s on %s\n", id.c_str(), parts[i].what, parts[j].what);
                ok = false;
            }
        }
    }
    if (ok) {
        // Square, with its quiet zone, every module whole pixels. A real card
        // keeps modules of three pixels or more, as every maintained screen had
        // before names were measured: a long name wraps rather than shrinking
        // the code a phone has to read.
        const lv_area_t qr = Coords(parts[0].obj);
        assert(lv_area_get_width(&qr) == lv_area_get_height(&qr));
        const int32_t module = ModulePixels(layer, qr);
        if (module < (named.real ? 3 : 2)) {
            std::fprintf(stderr, "%s: qr modules of %d px\n", id.c_str(), static_cast<int>(module));
            ok = false;
        }
    }
    if (!ok) Screenshot(layer, out_dir + "/FAILED-setup-code-" + id + ".ppm");
    assert(ok);

    // An unchanged card is not rebuilt when the window moves on.
    lv_obj_t* first_root = lv_obj_get_child(layer, 0);
    overlay.Show(display, screen);
    assert(lv_obj_get_child(layer, 0) == first_root);

    if (named.name == size.name && !with_notice) {
        Screenshot(layer, out_dir + "/setup-code-" + id + ".ppm");
    }
    overlay.Hide();
    assert(!overlay.visible());
    assert(lv_obj_get_child_count(layer) == 0);
    lv_display_delete(display);
}

// Everything the person needs is on screen, whole, and nothing sits on
// anything else: the QR code, the advertised name, the hint, both halves of
// the code, the notice and Cancel, on every maintained screen, whatever name
// the device advertises and whether or not the screen is a touch screen.
void TheCardFitsEveryMaintainedScreen(const std::string& out_dir)
{
    for (const Screen& size : kScreens) {
        for (const Card& named : Cards()) {
            for (const bool touch : {false, true}) {
                for (const bool with_notice : {false, true}) {
                    CheckCard(size, named, touch, with_notice, out_dir);
                }
            }
        }
    }
}

void NoCodeIsShownWithoutEightDigits()
{
    lv_display_t* display = lv_display_create(320, 240);
    static uint8_t pixels[320 * 20 * 2];
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    SetupCodeOverlay overlay;
    overlay.Show(display, SetupCodeScreen{"{}", "4071825", "eidolon-box3-a1b2c3", {}, nullptr});
    assert(!overlay.visible());
    lv_display_delete(display);
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    lv_init();
    TheCardFitsEveryMaintainedScreen(argv[1]);
    NoCodeIsShownWithoutEightDigits();
    return 0;
}
