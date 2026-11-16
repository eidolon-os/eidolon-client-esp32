#include "pet_face_skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>

namespace eidolon {

// Where part drawing goes: the LVGL layer (draw mode) or a recorder (rec != nullptr)
// that hashes the exact integer primitives a part would emit and unions their
// boxes. A part's change signature and dirty box come from the same code that
// draws it, so they cannot drift from what reaches the panel.
struct SkinRec {
    uint32_t h = 2166136261u;
    bool any = false;
    lv_area_t box{};
    bool clipped = false;
    lv_area_t clip{};
    void Mix(int32_t v) { h ^= static_cast<uint32_t>(v); h *= 16777619u; }
    void Mix(const lv_area_t& a) { Mix(a.x1); Mix(a.y1); Mix(a.x2); Mix(a.y2); }
    void Add(lv_area_t a, int32_t pad) {
        a.x1 -= pad; a.y1 -= pad; a.x2 += pad; a.y2 += pad;
        if (clipped) {
            a.x1 = std::max(a.x1, clip.x1); a.y1 = std::max(a.y1, clip.y1);
            a.x2 = std::min(a.x2, clip.x2); a.y2 = std::min(a.y2, clip.y2);
            if (a.x2 < a.x1 || a.y2 < a.y1) return;
        }
        if (!any) { box = a; any = true; return; }
        box.x1 = std::min(box.x1, a.x1); box.y1 = std::min(box.y1, a.y1);
        box.x2 = std::max(box.x2, a.x2); box.y2 = std::max(box.y2, a.y2);
    }
};
struct SkinCtx {
    lv_layer_t* layer;
    int32_t ox, oy;
    SkinRec* rec = nullptr;
};

namespace {

using pet::Frame;
constexpr float kPi = 3.14159265358979f;
float Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float Smooth(float x) { x = Clamp(x, 0, 1); return x * x * (3 - 2 * x); }
lv_opa_t Opa(float a) { return static_cast<lv_opa_t>(Clamp(a, 0, 1) * 255.f); }
uint32_t MixRgb(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { const float x = ((a >> s) & 255), y = ((b >> s) & 255); return static_cast<uint32_t>(x + (y - x) * t + .5f) & 255; };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
int32_t R(float v) { return static_cast<int32_t>(std::lround(v)); }

using Ctx = SkinCtx;

void Rect(Ctx& c, float x, float y, float w, float h, float radius, uint32_t rgb, lv_opa_t opa) {
    if (w < 1 || h < 1 || opa == 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = opa;
    d.radius = radius >= LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : R(radius);
    d.border_width = 0;
    lv_area_t a = {c.ox + R(x), c.oy + R(y), c.ox + R(x + w) - 1, c.oy + R(y + h) - 1};
    if (c.rec) { c.rec->Mix(1); c.rec->Mix(a); c.rec->Mix(d.radius); c.rec->Mix(static_cast<int32_t>(rgb)); c.rec->Mix(opa); c.rec->Add(a, 1); return; }
    lv_draw_rect(c.layer, &d, &a);
}
void GradRect(Ctx& c, float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom) {
    if (w < 1 || h < 1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = lv_color_hex(top);
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(top);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(bottom);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    d.radius = R(radius);
    d.border_width = 0;
    lv_area_t a = {c.ox + R(x), c.oy + R(y), c.ox + R(x + w) - 1, c.oy + R(y + h) - 1};
    if (c.rec) { c.rec->Mix(2); c.rec->Mix(a); c.rec->Mix(d.radius); c.rec->Mix(static_cast<int32_t>(top)); c.rec->Mix(static_cast<int32_t>(bottom)); c.rec->Add(a, 1); return; }
    lv_draw_rect(c.layer, &d, &a);
}
void Outline(Ctx& c, float x, float y, float w, float h, float radius, uint32_t fill, lv_opa_t fill_opa, uint32_t border, lv_opa_t border_opa) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(fill);
    d.bg_opa = fill_opa;
    d.radius = radius >= LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : R(radius);
    d.border_width = 2;
    d.border_color = lv_color_hex(border);
    d.border_opa = border_opa;
    lv_area_t a = {c.ox + R(x), c.oy + R(y), c.ox + R(x + w) - 1, c.oy + R(y + h) - 1};
    if (c.rec) {
        c.rec->Mix(3); c.rec->Mix(a); c.rec->Mix(d.radius); c.rec->Mix(static_cast<int32_t>(fill)); c.rec->Mix(fill_opa);
        c.rec->Mix(static_cast<int32_t>(border)); c.rec->Mix(border_opa); c.rec->Add(a, 1); return;
    }
    lv_draw_rect(c.layer, &d, &a);
}
void Circle(Ctx& c, float cx, float cy, float r, uint32_t rgb, lv_opa_t opa) {
    if (r < .5f) return;
    Rect(c, cx - r, cy - r, 2 * r, 2 * r, LV_RADIUS_CIRCLE, rgb, opa);
}
void Tri(Ctx& c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, lv_opa_t opa) {
    if (opa == 0) return;
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = c.ox + R(x0); d.p[0].y = c.oy + R(y0);
    d.p[1].x = c.ox + R(x1); d.p[1].y = c.oy + R(y1);
    d.p[2].x = c.ox + R(x2); d.p[2].y = c.oy + R(y2);
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    if (c.rec) {
        c.rec->Mix(4);
        for (const auto& p : d.p) { c.rec->Mix(static_cast<int32_t>(p.x)); c.rec->Mix(static_cast<int32_t>(p.y)); }
        c.rec->Mix(static_cast<int32_t>(rgb)); c.rec->Mix(opa);
        lv_area_t a = {static_cast<int32_t>(std::min({d.p[0].x, d.p[1].x, d.p[2].x})), static_cast<int32_t>(std::min({d.p[0].y, d.p[1].y, d.p[2].y})),
                       static_cast<int32_t>(std::max({d.p[0].x, d.p[1].x, d.p[2].x})), static_cast<int32_t>(std::max({d.p[0].y, d.p[1].y, d.p[2].y}))};
        c.rec->Add(a, 1);
        return;
    }
    lv_draw_triangle(c.layer, &d);
}
void Line(Ctx& c, float x0, float y0, float x1, float y1, float w, uint32_t rgb, lv_opa_t opa) {
    if (opa == 0 || w < .5f) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = c.ox + R(x0); d.p1.y = c.oy + R(y0);
    d.p2.x = c.ox + R(x1); d.p2.y = c.oy + R(y1);
    d.width = std::max<int32_t>(1, R(w));
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.round_start = 1;
    d.round_end = 1;
    if (c.rec) {
        c.rec->Mix(5); c.rec->Mix(static_cast<int32_t>(d.p1.x)); c.rec->Mix(static_cast<int32_t>(d.p1.y));
        c.rec->Mix(static_cast<int32_t>(d.p2.x)); c.rec->Mix(static_cast<int32_t>(d.p2.y)); c.rec->Mix(d.width);
        c.rec->Mix(static_cast<int32_t>(rgb)); c.rec->Mix(opa);
        lv_area_t a = {static_cast<int32_t>(std::min(d.p1.x, d.p2.x)), static_cast<int32_t>(std::min(d.p1.y, d.p2.y)),
                       static_cast<int32_t>(std::max(d.p1.x, d.p2.x)), static_cast<int32_t>(std::max(d.p1.y, d.p2.y))};
        c.rec->Add(a, d.width / 2 + 2);
        return;
    }
    lv_draw_line(c.layer, &d);
}
void Arc(Ctx& c, float cx, float cy, float r, float a0, float a1, float w, uint32_t rgb, lv_opa_t opa) {
    if (opa == 0 || r < 1) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = c.ox + R(cx);
    d.center.y = c.oy + R(cy);
    d.radius = static_cast<uint16_t>(R(r));
    d.start_angle = static_cast<int32_t>(a0);
    d.end_angle = static_cast<int32_t>(a1);
    d.width = std::max<int32_t>(1, R(w));
    d.rounded = 1;
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    if (c.rec) {
        c.rec->Mix(6); c.rec->Mix(d.center.x); c.rec->Mix(d.center.y); c.rec->Mix(d.radius); c.rec->Mix(d.start_angle); c.rec->Mix(d.end_angle);
        c.rec->Mix(d.width); c.rec->Mix(static_cast<int32_t>(rgb)); c.rec->Mix(opa);
        const lv_area_t a = {d.center.x - d.radius, d.center.y - d.radius, d.center.x + d.radius, d.center.y + d.radius};
        c.rec->Add(a, 2);
        return;
    }
    lv_draw_arc(c.layer, &d);
}
void Text(Ctx& c, const lv_font_t* font, float cx, float cy, const char* text, uint32_t rgb, lv_opa_t opa, float half_w = 60) {
    if (!font || !text || opa == 0) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.align = LV_TEXT_ALIGN_CENTER;
    const int32_t lh = lv_font_get_line_height(font);
    lv_area_t a = {c.ox + R(cx - half_w), c.oy + R(cy) - lh / 2, c.ox + R(cx + half_w), c.oy + R(cy) + lh / 2 + 1};
    if (c.rec) {
        c.rec->Mix(7); c.rec->Mix(a); c.rec->Mix(static_cast<int32_t>(rgb)); c.rec->Mix(opa);
        for (const char* t = text; *t; ++t) c.rec->Mix(static_cast<unsigned char>(*t));
        c.rec->Add(a, 1);
        return;
    }
    lv_draw_label(c.layer, &d, &a);
}

// Restricts later draw tasks to one eye so a lid never cuts the other eye.
// In record mode the recorder's boxes are clipped the same way.
class ClipScope {
public:
    ClipScope(Ctx& c, lv_area_t local) : c_(c) {
        lv_area_move(&local, c.ox, c.oy);
        if (c.rec) {
            saved_clipped_ = c.rec->clipped; saved_ = c.rec->clip;
            lv_area_t n = local;
            if (saved_clipped_) { n.x1 = std::max(n.x1, saved_.x1); n.y1 = std::max(n.y1, saved_.y1); n.x2 = std::min(n.x2, saved_.x2); n.y2 = std::min(n.y2, saved_.y2); }
            c.rec->clipped = true; c.rec->clip = n;
            ok_ = n.x1 <= n.x2 && n.y1 <= n.y2;
            return;
        }
        saved_ = c.layer->_clip_area;
        lv_area_t& clip = c.layer->_clip_area;
        clip.x1 = std::max(saved_.x1, local.x1); clip.y1 = std::max(saved_.y1, local.y1);
        clip.x2 = std::min(saved_.x2, local.x2); clip.y2 = std::min(saved_.y2, local.y2);
        ok_ = clip.x1 <= clip.x2 && clip.y1 <= clip.y2;
    }
    ~ClipScope() {
        if (c_.rec) { c_.rec->clipped = saved_clipped_; c_.rec->clip = saved_; }
        else c_.layer->_clip_area = saved_;
    }
    bool ok() const { return ok_; }
private:
    Ctx& c_;
    lv_area_t saved_{};
    bool saved_clipped_ = false;
    bool ok_ = false;
};

void HeartShape(Ctx& c, float x, float y, float s, uint32_t rgb, lv_opa_t opa) {
    Circle(c, x - s * .28f, y - s * .1f, s * .32f, rgb, opa);
    Circle(c, x + s * .28f, y - s * .1f, s * .32f, rgb, opa);
    Tri(c, x - s * .58f, y - s * .02f, x + s * .58f, y - s * .02f, x, y + s * .62f, rgb, opa);
}
void NoteShape(Ctx& c, float x, float y, float s, uint32_t rgb, lv_opa_t opa) {
    Circle(c, x, y, 4 * s, rgb, opa);
    Line(c, x + 3.5f * s, y, x + 3.5f * s, y - 14 * s, 2 * s, rgb, opa);
    Line(c, x + 3.5f * s, y - 14 * s, x + 9 * s, y - 10 * s, 2 * s, rgb, opa);
}

}  // namespace

void PetFaceSkin::Build(lv_obj_t* parent, const lv_font_t* font) {
    font_ = font;
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(obj_, 0, 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    bg_rgb_ = frame_.bg_rgb;
    lv_obj_set_style_bg_color(obj_, lv_color_hex(bg_rgb_), 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj_, OnDraw, LV_EVENT_DRAW_MAIN, this);
    if ((disp_ = lv_obj_get_display(obj_))) {
        lv_display_add_event_cb(disp_, OnRefresh, LV_EVENT_REFR_START, this);
        lv_display_add_event_cb(disp_, OnRefresh, LV_EVENT_REFR_READY, this);
    }
}

PetFaceSkin::~PetFaceSkin() {
    if (disp_) lv_display_remove_event_cb_with_user_data(disp_, OnRefresh, this);
}

void PetFaceSkin::OnRefresh(lv_event_t* e) {
    // One LVGL refresh = one frame on the panel (render + flush of every invalid area).
    auto* self = static_cast<PetFaceSkin*>(lv_event_get_user_data(e));
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        self->refresh_t0_ = now; self->frame_draws_ = self->stat_draws_;
        return;
    }
    if (self->stat_draws_ == self->frame_draws_ || !self->refresh_t0_) return;   // the pet was not in it
    const uint32_t us = static_cast<uint32_t>(now - self->refresh_t0_);
    ++self->stat_frames_;
    self->stat_frame_us_ += us;
    ++self->total_frames_;
    self->total_frame_us_ += us;
    self->stat_frame_us_max_ = std::max(self->stat_frame_us_max_, us);
}

void PetFaceSkin::TakeFrameStats(uint32_t& frames, uint32_t& frame_us_total, uint32_t& frame_us_max) {
    frames = stat_frames_; frame_us_total = stat_frame_us_; frame_us_max = stat_frame_us_max_;
    stat_frames_ = stat_frame_us_ = stat_frame_us_max_ = 0;
}

void PetFaceSkin::SetMini(bool mini) {
    if (mini == mini_) return;
    mini_ = mini;
    has_parts_ = false;
    lv_obj_invalidate(obj_);
}

void PetFaceSkin::SetCard(const char* line1, const char* line2) {
    std::snprintf(card_line1_, sizeof(card_line1_), "%s", line1 ? line1 : "");
    std::snprintf(card_line2_, sizeof(card_line2_), "%s", line2 ? line2 : "");
}

PetFaceSkin::Geo PetFaceSkin::Geometry(const Frame& f) const {
    const pet::Pose& P = f.pose;
    Geo g;
    // A card or caption at the bottom makes the face shrink as well as rise, so the
    // antenna tips stay below the status bar (y >= 32) at every shift.
    g.k = mini_ ? .5f : 1.f - .27f * f.face_shift;
    g.cx0 = pet::kFaceCx;
    g.cy0 = mini_ ? 62.f : pet::kFaceCy - 28.f * f.face_shift;
    const float s = 1 + .35f * P.scale;
    // Fast gaze moves would stretch the eyes; the runtime's springs already carry the
    // overshoot, so the device keeps the shape stable here (cheaper redraw).
    g.w = 62 * s * (1 + .22f * P.squash) * g.k;
    g.h = 80 * s * (1 - .28f * P.squash) * g.k;
    const float dx = 64 * g.k, cross = P.cross * 14 * g.k;
    const float growR = 1 + .12f * std::max(0.f, P.gaze_x) - .05f * std::max(0.f, -P.gaze_x);
    const float growL = 1 + .12f * std::max(0.f, -P.gaze_x) - .05f * std::max(0.f, P.gaze_x);
    const float sq = 1 + .22f * (1 - f.blink);
    g.eyes[0] = {g.cx0 - dx + P.gaze_x * 30 * g.k + cross, g.cy0 + P.gaze_y * 22 * g.k + P.roll * 12 * g.k,
                 g.w * (.5f + .5f * growL) * sq, g.h * growL, P.open_l * f.blink, P.lid_top_l, P.lid_angle_l, P.brow_l, -1};
    g.eyes[1] = {g.cx0 + dx + P.gaze_x2 * 30 * g.k - cross, g.cy0 + P.gaze_y2 * 22 * g.k - P.roll * 12 * g.k,
                 g.w * (.5f + .5f * growR) * sq, g.h * growR, P.open_r * f.blink, P.lid_top_r, P.lid_angle_r, P.brow_r, 1};
    return g;
}

uint32_t PetFaceSkin::Signature(const Frame& f) const {
    uint32_t h = 2166136261u;
    auto mix = [&](int32_t v) { h ^= static_cast<uint32_t>(v); h *= 16777619u; };
    const float* p = reinterpret_cast<const float*>(&f.pose);
    for (size_t i = 0; i < sizeof(pet::Pose) / sizeof(float); ++i) mix(R(p[i] * 60));
    mix(R(f.blink * 30)); mix(static_cast<int32_t>(f.glyph)); mix(R(f.glyph_k * 20)); mix(static_cast<int32_t>(f.mouth));
    mix(static_cast<int32_t>(f.emote)); mix(f.emote_age < .5f ? R(f.emote_age * 30) : (f.emote == pet::Emote::Dots ? R(f.emote_age * 10) : -1));
    mix(static_cast<int32_t>(f.bubble)); mix(R(f.bubble_alpha * 20)); mix(R(f.tip_l * 20)); mix(R(f.tip_r * 20)); mix(static_cast<int32_t>(f.tip_rgb));
    mix(R(f.twitch_l)); mix(R(f.twitch_r)); mix(R(f.card * 40)); mix(static_cast<int32_t>(f.eye_rgb));
    for (const auto& q : f.particles) if (q.alive) { mix(R(q.x)); mix(R(q.y)); mix(R(q.age * 20)); }
    mix(mini_ ? 1 : 0);
    return h;
}

PetFaceSkin::Antenna PetFaceSkin::AntennaGeometry(const Frame& f, const Geo& g, int side) const {
    const pet::Pose& P = f.pose;
    const float k = g.k;
    const float perk = side < 0 ? P.ear_l : P.ear_r;
    Antenna a;
    a.ax = g.cx0 + side * 96 * k;
    a.ay = g.cy0 - 50 * k + (side < 0 ? 1 : -1) * P.roll * 12 * k + (perk < 0 ? -perk * 8 * k : 0.f);
    const float out = 12 + (1 - perk) * 22 + (perk < 0 ? -perk * 22 : 0.f) + (side < 0 ? f.twitch_l : f.twitch_r);
    const float th = side * out * kPi / 180.f;
    const float L = 34 * k;
    a.mx = a.ax + std::sin(th) * L * .5f + side * 4 * k; a.my = a.ay - std::cos(th) * L * .5f;
    a.tx = a.ax + std::sin(th) * L; a.ty = a.ay - std::cos(th) * L;
    a.glow = side < 0 ? f.tip_l : f.tip_r;
    return a;
}

namespace {
bool Overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}
}  // namespace

void PetFaceSkin::Parts(const Frame& f, const Geo& g, PartBox out[kPartCount]) const {
    // Each part's box and signature are what its draw code would emit, recorded
    // in object-local integer coordinates (colours, opacities and text included).
    const auto record = [&](Part part, const auto& draw) {
        SkinRec rec;
        SkinCtx c{nullptr, 0, 0, &rec};
        rec.Mix(static_cast<int32_t>(f.bg_rgb));   // lids and cut-outs use the background
        draw(c);
        out[part].on = rec.any;
        out[part].a = rec.box;
        out[part].sig = rec.h;
    };
    for (int i = 0; i < 2; ++i)
        record(static_cast<Part>(kEyeL + i), [&](SkinCtx& c) { DrawEyeBody(c, f, g, i); DrawBrow(c, f, g, i); DrawCheek(c, f, g, i); });
    for (int i = 0; i < 2; ++i)
        record(static_cast<Part>(kAntennaL + i), [&](SkinCtx& c) { DrawAntenna(c, f, g, i == 0 ? -1 : 1); });
    record(kMouth, [&](SkinCtx& c) { DrawMouth(c, f, g); });
    record(kParticles, [&](SkinCtx& c) { DrawParticles(c, f); });
    record(kEmote, [&](SkinCtx& c) { DrawEmote(c, f, g); });
    record(kBubble, [&](SkinCtx& c) { DrawBubble(c, f, g); });
    record(kCard, [&](SkinCtx& c) { DrawCard(c, f); });
}

bool PetFaceSkin::Show(const Frame& f, bool force) {
    if (!obj_) return false;
    // Palette follows the room light; update the object background only on a visible step.
    const uint32_t d = std::abs(static_cast<int>((f.bg_rgb >> 16) & 255) - static_cast<int>((bg_rgb_ >> 16) & 255)) +
                       std::abs(static_cast<int>((f.bg_rgb >> 8) & 255) - static_cast<int>((bg_rgb_ >> 8) & 255)) +
                       std::abs(static_cast<int>(f.bg_rgb & 255) - static_cast<int>(bg_rgb_ & 255));
    if (d >= 4) { bg_rgb_ = f.bg_rgb; lv_obj_set_style_bg_color(obj_, lv_color_hex(bg_rgb_), 0); }
    const uint32_t sig = Signature(f);
    if (!force && sig == last_signature_) return false;
    last_signature_ = sig;
    const Geo g = Geometry(f);
    PartBox now[kPartCount];
    Parts(f, g, now);
    frame_ = f;
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    auto invalidate = [&](lv_area_t a) {
        a.x1 = std::max<int32_t>(a.x1, 0); a.y1 = std::max<int32_t>(a.y1, 0);
        a.x2 = std::min<int32_t>(a.x2, lv_obj_get_width(obj_) - 1); a.y2 = std::min<int32_t>(a.y2, lv_obj_get_height(obj_) - 1);
        if (a.x2 < a.x1 || a.y2 < a.y1) return false;
        lv_area_move(&a, coords.x1, coords.y1);
        lv_obj_invalidate_area(obj_, &a);
        return true;
    };
    bool any = false;
    if (!has_parts_) {
        lv_obj_invalidate(obj_);
        any = true;
    } else {
        // Only parts that changed, at their old and new place; LVGL merges overlaps.
        for (int i = 0; i < kPartCount; ++i) {
            if (now[i].on == parts_[i].on && (!now[i].on || now[i].sig == parts_[i].sig)) continue;
            if (parts_[i].on) any |= invalidate(parts_[i].a);
            if (now[i].on) any |= invalidate(now[i].a);
        }
    }
    for (int i = 0; i < kPartCount; ++i) parts_[i] = now[i];
    has_parts_ = true;
    if (!any && force) invalidate({150, 110, 170, 130});   // a receipt still needs a flush
    return true;
}

void PetFaceSkin::OnDraw(lv_event_t* e) {
    auto* self = static_cast<PetFaceSkin*>(lv_event_get_user_data(e));
    // Measures task creation; the software renderer executes the tasks right after.
    const int64_t t0 = esp_timer_get_time();
    self->Draw(lv_event_get_layer(e));
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    ++self->stat_draws_;
    self->stat_us_ += us;
    self->stat_us_max_ = std::max(self->stat_us_max_, us);
}

void PetFaceSkin::TakeStats(uint32_t& draws, uint32_t& draw_us_total, uint32_t& draw_us_max) {
    draws = stat_draws_; draw_us_total = stat_us_; draw_us_max = stat_us_max_;
    stat_draws_ = stat_us_ = stat_us_max_ = 0;
}

void PetFaceSkin::Draw(lv_layer_t* layer) {
    const Frame& f = frame_;
    const Geo g = Geometry(f);
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    SkinCtx c{layer, coords.x1, coords.y1, nullptr};
    // The band LVGL is rendering now, object-local: skip every part outside it.
    lv_area_t band = layer->_clip_area;
    lv_area_move(&band, -coords.x1, -coords.y1);
    const auto in = [&](int part) { return parts_[part].on && Overlap(parts_[part].a, band); };
    // Paint order: eyes, antennae (over the lids), brows, cheeks, mouth, particles,
    // emote, bubble, card.
    for (int i = 0; i < 2; ++i) if (in(kEyeL + i)) DrawEyeBody(c, f, g, i);
    for (int i = 0; i < 2; ++i) if (in(kAntennaL + i)) DrawAntenna(c, f, g, i == 0 ? -1 : 1);
    for (int i = 0; i < 2; ++i) if (in(kEyeL + i)) DrawBrow(c, f, g, i);
    for (int i = 0; i < 2; ++i) if (in(kEyeL + i)) DrawCheek(c, f, g, i);
    if (in(kMouth)) DrawMouth(c, f, g);
    if (in(kParticles)) DrawParticles(c, f);
    if (in(kEmote)) DrawEmote(c, f, g);
    if (in(kBubble)) DrawBubble(c, f, g);
    if (in(kCard)) DrawCard(c, f);
}

void PetFaceSkin::DrawEyeBody(SkinCtx& c, const Frame& f, const Geo& g, int index) const {
    const pet::Pose& P = f.pose;
    const uint32_t eye = f.eye_rgb, bg = f.bg_rgb;
    const float lw = std::max(6.f, g.w * .15f);
    const auto& e = g.eyes[index];
    const float w = e.ew, h = e.eh;
    const float m = std::min(w, h);
    if (f.glyph != pet::Glyph::None) {
        const float kq = f.glyph_k;
        if (kq < .6f) { Rect(c, e.cx - w * .42f, e.cy - 3, w * .84f, 6, 3, eye, LV_OPA_COVER); return; }
        switch (f.glyph) {
        case pet::Glyph::Heart: HeartShape(c, e.cx, e.cy, m * .66f, eye, LV_OPA_COVER); break;
        case pet::Glyph::Star: {
            float vx[10], vy[10];
            for (int i = 0; i < 10; ++i) { const float a = -kPi / 2 + i * kPi / 5, rad = i % 2 ? m * .25f : m * .56f; vx[i] = e.cx + std::cos(a) * rad; vy[i] = e.cy + std::sin(a) * rad; }
            for (int i = 0; i < 10; ++i) Tri(c, e.cx, e.cy, vx[i], vy[i], vx[(i + 1) % 10], vy[(i + 1) % 10], eye, LV_OPA_COVER);
            break; }
        case pet::Glyph::Spiral: {
            float px = e.cx + 2, py = e.cy;
            for (float a = .35f; a <= kPi * 3.4f; a += .35f) {
                const float rad = 2 + a * m * .045f, x = e.cx + std::cos(a * e.side) * rad, y = e.cy + std::sin(a * e.side) * rad;
                Line(c, px, py, x, y, std::max(4.f, w * .09f), eye, LV_OPA_COVER); px = x; py = y;
            }
            break; }
        case pet::Glyph::Happy: Arc(c, e.cx, e.cy + h * .2f, w * .36f, 207, 333, lw, eye, LV_OPA_COVER); break;
        case pet::Glyph::Content: Arc(c, e.cx, e.cy - h * .14f, w * .34f, 27, 153, lw * .9f, eye, LV_OPA_COVER); break;
        case pet::Glyph::Squeeze: {
            const float dd = e.side < 0 ? 1.f : -1.f;
            Line(c, e.cx - dd * w * .28f, e.cy - h * .24f, e.cx + dd * w * .26f, e.cy, lw, eye, LV_OPA_COVER);
            Line(c, e.cx + dd * w * .26f, e.cy, e.cx - dd * w * .28f, e.cy + h * .24f, lw, eye, LV_OPA_COVER);
            break; }
        case pet::Glyph::Line: Rect(c, e.cx - w * .42f, e.cy - 4, w * .84f, 8, 4, eye, LV_OPA_COVER); break;
        case pet::Glyph::X: {
            const float a = m * .27f;
            Line(c, e.cx - a, e.cy - a, e.cx + a, e.cy + a, lw * .9f, eye, LV_OPA_COVER);
            Line(c, e.cx + a, e.cy - a, e.cx - a, e.cy + a, lw * .9f, eye, LV_OPA_COVER);
            break; }
        default: break;
        }
        return;
    }
    const float open = e.open * f.glyph_k;
    if (open < .08f) {   // closed: a soft arc
        Arc(c, e.cx, e.cy - w * .3f, w * .45f, 30, 150, std::max(3.f, w * .1f), eye, LV_OPA_COVER);
        return;
    }
    const float hv = std::max(3.f, h * open);
    const float top = e.cy - hv / 2 + (h - hv) * .08f, left = e.cx - w / 2;
    const float r = P.round * std::min(w, hv) / 2;
    // Glow: two soft shells instead of a blur. Only the plain background lies under
    // them (eyes are painted first), so their translucent colours are mixed here
    // and filled opaque: identical on screen, about 3x cheaper than blending (the
    // shells were two thirds of the eye's render time on the device).
    const uint32_t shell_out = MixRgb(bg, eye, 22.f / 255.f), shell_in = MixRgb(shell_out, eye, 44.f / 255.f);
    Rect(c, left - 7, top - 7, w + 14, hv + 14, r + 7, shell_out, LV_OPA_COVER);
    Rect(c, left - 3, top - 3, w + 6, hv + 6, r + 3, shell_in, LV_OPA_COVER);
    GradRect(c, left, top, w, hv, r, MixRgb(eye, 0xFFFFFF, .3f), MixRgb(eye, bg, .28f));
    if (P.glint > .01f) {
        Circle(c, e.cx + w * .2f - P.gaze_x * 5, top + hv * .25f, std::max(2.4f, w * .095f), 0xFFFFFF, Opa(.62f * P.glint));
        Circle(c, e.cx + w * .04f - P.gaze_x * 4, top + hv * .42f, std::max(1.2f, w * .042f), 0xFFFFFF, Opa(.32f * P.glint));
    }
    // Lids in background colour, clipped to this eye.
    const lv_area_t box = {R(left - 8), R(top - 8), R(left + w + 8), R(top + hv + 8)};
    ClipScope clip(c, box);
    if (clip.ok()) {
        if (e.lt > .001f || std::fabs(e.la) > .5f) {
            const float yc = top + e.lt * hv, dir = e.side < 0 ? 1.f : -1.f;
            const float kk = std::tan(e.la * kPi / 180.f) * dir, ext = w / 2 + 8;
            // One triangle whose lower edge is the lid line, extended far past the
            // eye and clipped to it: two triangles would leave an anti-aliased seam.
            const float sl = kk * ext / (w / 2 + 8);   // slope of the lid line
            const float reach = w + 160;
            Tri(c, e.cx - reach, yc - sl * reach, e.cx + reach, yc + sl * reach, e.cx, top - 4 * reach, bg, LV_OPA_COVER);
        }
        if (P.lid_bottom > .001f) {
            const float rad = w * .8f;
            Circle(c, e.cx, top + hv + rad - P.lid_bottom * hv * .88f, rad, bg, LV_OPA_COVER);
        }
    }
}

void PetFaceSkin::DrawAntenna(SkinCtx& c, const Frame& f, const Geo& g, int side) const {
    // The default sprite body; drawn after the eyes so a lid never paints over a base.
    const uint32_t eye = f.eye_rgb, bg = f.bg_rgb;
    const float k = g.k;
    const Antenna an = AntennaGeometry(f, g, side);
    const float ax = an.ax, ay = an.ay, mx = an.mx, my = an.my, tx = an.tx, ty = an.ty;
    const float glow = an.glow;
    const uint32_t tip = f.tip_rgb ? f.tip_rgb : eye;
    // The halo sits on plain background above the eyes: mixed and filled opaque, and
    // painted first so the stalk still reaches into it. The stalk is opaque too (it
    // was 70% eye over background); the base pill stays translucent because it
    // overlaps the top of the eye glow.
    Circle(c, tx, ty - 4 * k, (6.5f + 6 * glow) * k, MixRgb(bg, tip, .12f + .25f * glow), LV_OPA_COVER);
    Rect(c, ax - 10 * k, ay + 1 * k, 20 * k, 9 * k, LV_RADIUS_CIRCLE, eye, 60);
    const uint32_t stalk = MixRgb(bg, eye, 180.f / 255.f);
    Line(c, ax, ay + 3 * k, mx, my, 3 * k, stalk, LV_OPA_COVER);
    Line(c, mx, my, tx, ty, 3 * k, stalk, LV_OPA_COVER);
    Circle(c, tx, ty - 4 * k, 6.5f * k, MixRgb(bg, tip, .45f + .55f * glow), LV_OPA_COVER);
}

void PetFaceSkin::DrawBrow(SkinCtx& c, const Frame& f, const Geo& g, int index) const {
    // Only for strong feelings.
    const pet::Pose& P = f.pose;
    const auto& e = g.eyes[index];
    const float k = g.k;
    const float vis = Clamp(std::fabs(e.brow) * 2.4f + std::fabs(P.brow_angle) / 14.f, 0, 1);
    if (vis < .03f) return;
    const float y = e.cy - e.eh * .5f - 15 * k - e.brow * 9 * k;
    const float th = (e.side < 0 ? 1.f : -1.f) * P.brow_angle * kPi / 180.f;
    const float hx = 17 * k * std::cos(th), hy = 17 * k * std::sin(th);
    Line(c, e.cx - hx, y - hy, e.cx + hx, y + hy, 5.4f * k, f.eye_rgb, Opa(vis * .9f));
}

void PetFaceSkin::DrawCheek(SkinCtx& c, const Frame& f, const Geo& g, int index) const {
    const pet::Pose& P = f.pose;
    if (P.blush <= .01f) return;
    const auto& e = g.eyes[index];
    const float k = g.k;
    Rect(c, e.cx + e.side * 6 * k - 16 * k, e.cy + g.h * .5f + 6 * k, 32 * k, 14 * k, LV_RADIUS_CIRCLE, 0xFF809C, Opa(.5f * P.blush));
}

void PetFaceSkin::DrawMouth(SkinCtx& c, const Frame& f, const Geo& g) const {
    const pet::Pose& P = f.pose;
    if (f.mouth == pet::Mouth::None || P.mouth_size <= .03f) return;
    const uint32_t eye = f.eye_rgb;
    const float k = g.k;
    const float m = P.mouth_size, x = g.cx0 + P.gaze_x * 12 * k, y = g.cy0 + 64 * k + P.gaze_y * 8 * k;
    switch (f.mouth) {
    case pet::Mouth::O: Rect(c, x - (4 + 6 * m) * k, y - (5 + 8 * m) * k, (8 + 12 * m) * k, (10 + 16 * m) * k, LV_RADIUS_CIRCLE, eye, LV_OPA_COVER); break;
    case pet::Mouth::W: { const float rr = (4 + 3 * m) * k; Arc(c, x - rr, y, rr, 0, 180, 3 * k, eye, LV_OPA_COVER); Arc(c, x + rr, y, rr, 0, 180, 3 * k, eye, LV_OPA_COVER); break; }
    case pet::Mouth::V: { const float hw = 10 * (.6f + m) * k, dh = 14 * (.5f + m) * k;
        Tri(c, x - hw, y - 4 * k, x + hw, y - 4 * k, x, y + dh, eye, LV_OPA_COVER); Circle(c, x, y + dh * .45f, hw * .55f, eye, LV_OPA_COVER); break; }
    case pet::Mouth::Wave: { float px = x - 12 * k, py = y;
        for (int i = 1; i <= 12; ++i) { const float nx = x - 12 * k + i * 2 * k, ny = y + std::sin(i / 12.f * kPi * 3) * 2.5f * k; Line(c, px, py, nx, ny, 3 * k, eye, LV_OPA_COVER); px = nx; py = ny; }
        break; }
    default: break;
    }
}

void PetFaceSkin::DrawParticles(SkinCtx& c, const Frame& f) const {
    const uint32_t eye = f.eye_rgb;
    for (const auto& q : f.particles) {
        if (!q.alive) continue;
        const lv_opa_t a = Opa(std::sin(Clamp(q.age / q.life, 0, 1) * kPi) * 1.2f);
        switch (q.kind) {
        case pet::ParticleKind::Heart: HeartShape(c, q.x + std::sin(q.age * 4 + q.phase) * 6, q.y, 13 * q.size, 0xFF8FA8, a); break;
        case pet::ParticleKind::Spark: { const float r = 7 * q.size;
            Tri(c, q.x, q.y - r, q.x + r * .28f, q.y, q.x, q.y + r, eye, a); Tri(c, q.x, q.y - r, q.x - r * .28f, q.y, q.x, q.y + r, eye, a);
            Tri(c, q.x - r, q.y, q.x, q.y - r * .28f, q.x + r, q.y, eye, a); Tri(c, q.x - r, q.y, q.x, q.y + r * .28f, q.x + r, q.y, eye, a); break; }
        case pet::ParticleKind::Note: NoteShape(c, q.x + std::sin(q.age * 3 + q.phase) * 8, q.y, q.size, eye, a); break;
        case pet::ParticleKind::Zz: Text(c, font_, q.x, q.y, "Z", eye, a, 20); break;
        case pet::ParticleKind::Sweat: Circle(c, q.x, q.y + 2, 4, 0x9CC8FF, a); Tri(c, q.x, q.y - 9, q.x - 4, q.y + 1, q.x + 4, q.y + 1, 0x9CC8FF, a); break;
        case pet::ParticleKind::Dust: Circle(c, q.x, q.y, 3 + 5 * (q.age / q.life) * q.size, eye, Opa(.35f * a / 255.f)); break;
        }
    }
}

void PetFaceSkin::DrawEmote(SkinCtx& c, const Frame& f, const Geo& g) const {
    if (f.emote == pet::Emote::None) return;
    const uint32_t eye = f.eye_rgb;
    const float k = g.k;
    const float x = g.cx0 + 128 * k, y = g.cy0 - 26 * k, s = Smooth(std::min(1.f, f.emote_age * 6)) * k;
    switch (f.emote) {
    case pet::Emote::Bang: Rect(c, x - 4 * s, y - 20 * s, 8 * s, 24 * s, 4 * s, eye, LV_OPA_COVER); Circle(c, x, y + 13 * s, 4.5f * s, eye, LV_OPA_COVER); break;
    case pet::Emote::Question: Text(c, font_, x, y, "?", eye, LV_OPA_COVER, 20); break;
    case pet::Emote::Anger: {
        const uint32_t col = 0xFF9E7A;
        Line(c, x - 9 * s, y - 12 * s, x - 3 * s, y - 2 * s, 3.2f * s, col, LV_OPA_COVER); Line(c, x + 9 * s, y - 12 * s, x + 3 * s, y - 2 * s, 3.2f * s, col, LV_OPA_COVER);
        Line(c, x - 9 * s, y + 10 * s, x - 3 * s, y + 2 * s, 3.2f * s, col, LV_OPA_COVER); Line(c, x + 9 * s, y + 10 * s, x + 3 * s, y + 2 * s, 3.2f * s, col, LV_OPA_COVER); break; }
    case pet::Emote::Dots:
        for (int i = 0; i < 3; ++i) { const float b = std::max(0.f, std::sin((f.emote_age * 3 - i * .35f) * kPi)); Circle(c, x - 11 * k + i * 11 * k, y - 4 * b * k, 3.4f * k, eye, Opa(.45f + .55f * b)); }
        break;
    default: break;
    }
}

void PetFaceSkin::DrawBubble(SkinCtx& c, const Frame& f, const Geo& g) const {
    // Thought bubble: outside facts without words.
    if (f.bubble == pet::Bubble::None || f.bubble_alpha <= .02f) return;
    const uint32_t eye = f.eye_rgb, bg = f.bg_rgb;
    const float k = g.k;
    const lv_opa_t a = Opa(f.bubble_alpha);
    const float bx = g.cx0 + 48 * k, by = g.cy0 - 78 * k;
    Circle(c, g.cx0 + 26 * k, g.cy0 - 56 * k, 4 * k, eye, Opa(.25f * f.bubble_alpha));
    Circle(c, g.cx0 + 19 * k, g.cy0 - 46 * k, 2.5f * k, eye, Opa(.25f * f.bubble_alpha));
    Outline(c, bx - 24 * k, by - 18 * k, 48 * k, 36 * k, LV_RADIUS_CIRCLE, eye, Opa(.1f * f.bubble_alpha), eye, Opa(.5f * f.bubble_alpha));
    switch (f.bubble) {
    case pet::Bubble::Moon: Circle(c, bx, by, 9 * k, eye, a); Circle(c, bx + 4 * k, by - 2 * k, 7 * k, MixRgb(bg, eye, .1f), a); break;
    case pet::Bubble::Bell: Tri(c, bx - 8 * k, by + 5 * k, bx, by - 9 * k, bx + 8 * k, by + 5 * k, eye, a); Circle(c, bx, by + 8 * k, 2.2f * k, eye, a); break;
    case pet::Bubble::Battery:
        Outline(c, bx - 9 * k, by - 5 * k, 16 * k, 10 * k, 2, bg, 0, eye, a); Rect(c, bx + 7 * k, by - 2 * k, 2 * k, 4 * k, 0, eye, a); Rect(c, bx - 7 * k, by - 3 * k, 3 * k, 6 * k, 0, 0xFFB547, a); break;
    case pet::Bubble::Bolt:
        Tri(c, bx + 2 * k, by - 10 * k, bx - 6 * k, by + 2 * k, bx, by + 2 * k, eye, a); Tri(c, bx - 2 * k, by + 10 * k, bx + 6 * k, by - 2 * k, bx, by - 2 * k, eye, a); break;
    case pet::Bubble::Thermo: Rect(c, bx - 2.5f * k, by - 10 * k, 5 * k, 14 * k, 2.5f * k, eye, a); Circle(c, bx, by + 6 * k, 4 * k, eye, a); break;
    case pet::Bubble::Note: NoteShape(c, bx - 3 * k, by + 5 * k, k, eye, a); break;
    default: break;
    }
}

void PetFaceSkin::DrawCard(SkinCtx& c, const Frame& f) const {
    // Glance card, two stacked lines, left of the bottom-right corner.
    if (f.card <= .01f || mini_) return;
    const float y = 240 - f.card * 80;
    Outline(c, 10, y, 232, 68, 14, 0xFFFFFF, 20, f.eye_rgb, 64);
    Text(c, font_, 126, y + 22, card_line1_, 0xEEE9DE, LV_OPA_COVER, 110);
    Text(c, font_, 126, y + 46, card_line2_, 0xEEE9DE, Opa(.75f), 110);
}

}  // namespace eidolon
