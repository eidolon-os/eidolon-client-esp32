#include <lvgl.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

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

lv_obj_t* FindQr(lv_obj_t* obj)
{
    if (lv_obj_check_type(obj, &lv_canvas_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (auto* found = FindQr(lv_obj_get_child(obj, i))) return found;
    }
    return nullptr;
}

lv_area_t Coords(lv_obj_t* obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

void AssertOnScreen(lv_obj_t* obj, int32_t width, int32_t height)
{
    const lv_area_t a = Coords(obj);
    if (!(a.x1 >= 0 && a.y1 >= 0 && a.x2 < width && a.y2 < height)) {
        std::fprintf(stderr, "off screen: (%d,%d)-(%d,%d) on %dx%d\n", static_cast<int>(a.x1),
                     static_cast<int>(a.y1), static_cast<int>(a.x2), static_cast<int>(a.y2),
                     static_cast<int>(width), static_cast<int>(height));
    }
    assert(a.x1 >= 0 && a.y1 >= 0 && a.x2 < width && a.y2 < height);
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

// Every display a maintained board has: the QR code, the advertised name and
// both halves of the code fit on screen, and the digits never sit on the code.
void TheCodeFitsEveryMaintainedScreen(const std::string& out_dir)
{
    const auto card = eidolon::MakeSetupCodeCard("40718256", "eidolon-box3-a1b2c3",
                                                 df::SetupQrTransport::Ble);
    const SetupCodeScreen screen{card.qr_json, card.code, card.advertised_name, {}};
    const int32_t sizes[][2] = {{320, 240}, {360, 360}, {410, 502}, {800, 480}};
    for (const auto& size : sizes) {
        lv_display_t* display = lv_display_create(size[0], size[1]);
        static uint8_t pixels[800 * 20 * 2];
        lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
            lv_display_flush_ready(d);
        });
        lv_display_set_default(display);

        SetupCodeOverlay overlay;
        overlay.Show(display, screen);
        assert(overlay.visible());
        lv_obj_t* layer = lv_display_get_layer_top(display);
        lv_obj_update_layout(layer);

        lv_obj_t* qr = FindQr(layer);
        assert(qr != nullptr);
        AssertOnScreen(qr, size[0], size[1]);
        const lv_area_t qr_area = Coords(qr);
        assert(lv_area_get_width(&qr_area) >= 96);
        for (const char* text : {"4071", "8256", "eidolon-box3-a1b2c3"}) {
            lv_obj_t* label = FindLabel(layer, text);
            assert(label != nullptr);
            AssertOnScreen(label, size[0], size[1]);
            const lv_area_t label_area = Coords(label);
            assert(!Overlap(label_area, qr_area));
        }

        // An unchanged card is not rebuilt when the window moves on.
        lv_obj_t* first_root = lv_obj_get_child(layer, 0);
        overlay.Show(display, screen);
        assert(lv_obj_get_child(layer, 0) == first_root);

        Screenshot(layer, out_dir + "/setup-code-" + std::to_string(size[0]) + "x" +
                              std::to_string(size[1]) + ".ppm");
        overlay.Hide();
        assert(!overlay.visible());
        assert(lv_obj_get_child_count(layer) == 0);
        lv_display_delete(display);
    }
}

void NoCodeIsShownWithoutEightDigits()
{
    lv_display_t* display = lv_display_create(320, 240);
    static uint8_t pixels[320 * 20 * 2];
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    SetupCodeOverlay overlay;
    overlay.Show(display, SetupCodeScreen{"{}", "4071825", "eidolon-box3-a1b2c3", {}});
    assert(!overlay.visible());
    lv_display_delete(display);
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    lv_init();
    TheCodeFitsEveryMaintainedScreen(argv[1]);
    NoCodeIsShownWithoutEightDigits();
    return 0;
}
