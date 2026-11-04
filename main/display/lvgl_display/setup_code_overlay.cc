#include "setup_code_overlay.h"

#include <algorithm>
#include <cstdint>

LV_FONT_DECLARE(font_home_clock_34);

// Nayuki's QR Code generator, already linked into every build: esp_emote_gfx
// ships it, and LVGL's copy (behind LV_USE_QRCODE) would define the same
// symbols twice. Declared here rather than included, because neither copy
// publishes its header; the API and these enum values are the library's
// stable ones.
extern "C" {
enum qrcodegen_Ecc { qrcodegen_Ecc_LOW = 0, qrcodegen_Ecc_MEDIUM = 1 };
enum qrcodegen_Mask { qrcodegen_Mask_AUTO = -1 };
bool qrcodegen_encodeText(const char* text, uint8_t tempBuffer[], uint8_t qrcode[],
                          enum qrcodegen_Ecc ecl, int minVersion, int maxVersion,
                          enum qrcodegen_Mask mask, bool boostEcl);
int qrcodegen_getSize(const uint8_t qrcode[]);
bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y);
}

namespace {

constexpr int32_t kMargin = 12;
constexpr int32_t kNameHeight = 18;
// Wide enough for four 34 px digits.
constexpr int32_t kCodeColumnWidth = 100;
constexpr int32_t kCodeBlockHeight = 120;
// Beyond this the code is no easier to scan, only harder to fit beside text.
constexpr int32_t kMaxQrSide = 320;
// A setup payload is about 120 bytes: version 7 or 8 at medium error
// correction. Version 10 leaves room without letting a runaway string through.
constexpr int kMaxQrVersion = 10;
constexpr size_t kQrBufferLen = ((kMaxQrVersion * 4 + 17) * (kMaxQrVersion * 4 + 17) + 7) / 8 + 1;
// Quiet zone the standard asks for, in modules, on every side.
constexpr int kQuietModules = 4;

void DestroyDrawBuf(lv_event_t* event)
{
    lv_draw_buf_destroy(static_cast<lv_draw_buf_t*>(lv_event_get_user_data(event)));
}

// Draw `text` as a QR code at most `side` pixels square, every module a whole
// number of pixels and the quiet zone included. Null if it cannot be drawn
// sharply enough to scan; the digits beside it still let a person type it.
lv_obj_t* DrawQrCode(lv_obj_t* parent, const std::string& text, int32_t side)
{
    static uint8_t qr[kQrBufferLen];
    static uint8_t scratch[kQrBufferLen];
    if (!qrcodegen_encodeText(text.c_str(), scratch, qr, qrcodegen_Ecc_MEDIUM, 1,
                              kMaxQrVersion, qrcodegen_Mask_AUTO, true)) {
        return nullptr;
    }
    const int modules = qrcodegen_getSize(qr);
    const int total = modules + 2 * kQuietModules;
    const int scale = side / total;
    if (scale < 2) return nullptr;
    const int32_t pixels = scale * total;

    lv_draw_buf_t* buffer =
        lv_draw_buf_create(pixels, pixels, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
    if (buffer == nullptr) return nullptr;
    lv_obj_t* canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas, buffer);
    lv_obj_add_event_cb(canvas, DestroyDrawBuf, LV_EVENT_DELETE, buffer);
    // Index 0 light, 1 dark, whatever the theme.
    lv_canvas_set_palette(canvas, 0, lv_color32_make(0xff, 0xff, 0xff, 0xff));
    lv_canvas_set_palette(canvas, 1, lv_color32_make(0x00, 0x00, 0x00, 0xff));

    // I1: two palette entries (8 bytes), then rows of 1-bit pixels, MSB first.
    uint8_t* rows = buffer->data + 8;
    const uint32_t stride = buffer->header.stride;
    const int margin = kQuietModules * scale;
    for (int32_t y = 0; y < pixels; ++y) {
        uint8_t* row = rows + stride * y;
        for (uint32_t i = 0; i < stride; ++i) row[i] = 0;
        if (y < margin || y >= pixels - margin) continue;
        const int module_y = (y - margin) / scale;
        for (int32_t x = margin; x < pixels - margin; ++x) {
            if (qrcodegen_getModule(qr, (x - margin) / scale, module_y)) {
                row[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
            }
        }
    }
    lv_obj_invalidate(canvas);
    return canvas;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

lv_obj_t* Column(lv_obj_t* parent)
{
    lv_obj_t* column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, 6, 0);
    lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return column;
}

}  // namespace

void SetupCodeOverlay::Show(lv_display_t* display, const SetupCodeScreen& screen)
{
    if (root_ != nullptr && shown_qr_text_ == screen.qr_text && shown_notice_ == screen.notice) {
        return;
    }
    Hide();
    if (display == nullptr || screen.qr_text.empty() || screen.code.size() != 8) return;

    const int32_t width = lv_display_get_horizontal_resolution(display);
    const int32_t height = lv_display_get_vertical_resolution(display);
    const bool landscape = width >= height;
    const int32_t qr_side = std::min<int32_t>(
        kMaxQrSide,
        landscape ? std::min(height - 2 * kMargin - kNameHeight,
                             width - 3 * kMargin - kCodeColumnWidth)
                  : std::min(width - 2 * kMargin,
                             height - 3 * kMargin - kNameHeight - kCodeBlockHeight));
    if (qr_side < 96) return;

    root_ = lv_obj_create(lv_display_get_layer_top(display));
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, width, height);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, kMargin, 0);
    lv_obj_set_style_pad_column(root_, kMargin, 0);
    lv_obj_set_style_pad_row(root_, kMargin, 0);
    lv_obj_set_flex_flow(root_, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    // Taps land on the overlay rather than on a view underneath it.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    shown_qr_text_ = screen.qr_text;
    shown_notice_ = screen.notice;
    lv_obj_t* qr_column = Column(root_);
    DrawQrCode(qr_column, screen.qr_text, qr_side);
    Label(qr_column, screen.advertised_name.c_str(), &lv_font_montserrat_14);

    lv_obj_t* code_column = Column(root_);
    lv_obj_t* hint = Label(code_column, "Scan with the Eidolon app, or enter",
                           &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, landscape ? kCodeColumnWidth : width - 2 * kMargin);
    Label(code_column, screen.code.substr(0, 4).c_str(), &font_home_clock_34);
    Label(code_column, screen.code.substr(4, 4).c_str(), &font_home_clock_34);
    if (!screen.notice.empty()) {
        lv_obj_t* notice = Label(code_column, screen.notice.c_str(), &lv_font_montserrat_14);
        lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(notice, landscape ? kCodeColumnWidth : width - 2 * kMargin);
    }
}

void SetupCodeOverlay::Hide()
{
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    for (char& c : shown_qr_text_) c = '\0';
    shown_qr_text_.clear();
    shown_notice_.clear();
}
