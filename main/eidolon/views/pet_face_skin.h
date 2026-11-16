#pragma once

// Draws a pet::Frame with LVGL draw primitives on one full-screen object.
// No canvas, no object transforms (LVGL transforms allocate whole layers): every
// shape is computed and emitted as rect / triangle / line / arc / label tasks in
// the object's DRAW_MAIN event, and only the area that changed is invalidated.
// Style: "glaze" (design §4.11) with the default sprite (antenna) body.

#include <lvgl.h>

#include "eidolon/pet/pet_face.h"

namespace eidolon {

struct SkinCtx;

class PetFaceSkin {
public:
    PetFaceSkin() = default;
    PetFaceSkin(const PetFaceSkin&) = delete;
    PetFaceSkin& operator=(const PetFaceSkin&) = delete;
    ~PetFaceSkin();
    void Build(lv_obj_t* parent, const lv_font_t* font);
    lv_obj_t* obj() const { return obj_; }
    void SetFont(const lv_font_t* font) { font_ = font; }
    // mini = system pages: a small pet at the top, text below.
    void SetMini(bool mini);
    // Store the frame and invalidate what changed. force=true always produces a flush
    // (an expression receipt needs a real frame boundary). Returns true if anything
    // was invalidated.
    bool Show(const pet::Frame& frame, bool force);
    // Text of the glance card (double tap on the head); copied, drawn while card > 0.
    void SetCard(const char* line1, const char* line2);
    // Draw cost since the last call (for the periodic self-check log).
    void TakeStats(uint32_t& draws, uint32_t& draw_us_total, uint32_t& draw_us_max);
    // Frames that reached the panel since the last call and their render+flush time.
    void TakeFrameStats(uint32_t& frames, uint32_t& frame_us_total, uint32_t& frame_us_max);
    // Running totals since construction (never reset), for attributing frames to a phase.
    void FrameTotals(uint32_t& frames, uint64_t& frame_us) const { frames = total_frames_; frame_us = total_frame_us_; }

private:
    struct Geo {
        float k, cx0, cy0, w, h;
        struct Eye { float cx, cy, ew, eh, open, lt, la, brow; int side; } eyes[2];
    };
    // The face as separate parts, each with its own box (object-local) and change
    // signature: Show invalidates only parts that changed, Draw skips parts outside
    // the band being rendered (the panel is drawn in 10-line bands).
    enum Part { kEyeL, kEyeR, kAntennaL, kAntennaR, kMouth, kEmote, kBubble, kParticles, kCard, kPartCount };
    struct PartBox { lv_area_t a{}; uint32_t sig = 0; bool on = false; };
    struct Antenna { float ax, ay, mx, my, tx, ty, glow; };
    static void OnDraw(lv_event_t* e);
    static void OnRefresh(lv_event_t* e);
    void Draw(lv_layer_t* layer);
    Geo Geometry(const pet::Frame& f) const;
    Antenna AntennaGeometry(const pet::Frame& f, const Geo& g, int side) const;
    void Parts(const pet::Frame& f, const Geo& g, PartBox out[kPartCount]) const;
    // One function per part, used both to draw (into the LVGL layer) and to record
    // (signature + box) — see SkinRec in the .cc.
    void DrawEyeBody(SkinCtx& c, const pet::Frame& f, const Geo& g, int index) const;
    void DrawBrow(SkinCtx& c, const pet::Frame& f, const Geo& g, int index) const;
    void DrawCheek(SkinCtx& c, const pet::Frame& f, const Geo& g, int index) const;
    void DrawAntenna(SkinCtx& c, const pet::Frame& f, const Geo& g, int side) const;
    void DrawMouth(SkinCtx& c, const pet::Frame& f, const Geo& g) const;
    void DrawParticles(SkinCtx& c, const pet::Frame& f) const;
    void DrawEmote(SkinCtx& c, const pet::Frame& f, const Geo& g) const;
    void DrawBubble(SkinCtx& c, const pet::Frame& f, const Geo& g) const;
    void DrawCard(SkinCtx& c, const pet::Frame& f) const;
    uint32_t Signature(const pet::Frame& f) const;

    lv_obj_t* obj_ = nullptr;
    lv_display_t* disp_ = nullptr;   // refresh events feed the frame statistics
    const lv_font_t* font_ = nullptr;
    pet::Frame frame_;
    bool mini_ = false;
    bool has_parts_ = false;
    PartBox parts_[kPartCount];
    uint32_t last_signature_ = 0;
    uint32_t bg_rgb_ = 0;
    uint32_t stat_draws_ = 0, stat_us_ = 0, stat_us_max_ = 0;
    uint32_t frame_draws_ = 0;             // draws seen in the refresh under way
    int64_t refresh_t0_ = 0;
    uint32_t stat_frames_ = 0, stat_frame_us_ = 0, stat_frame_us_max_ = 0;
    uint32_t total_frames_ = 0;
    uint64_t total_frame_us_ = 0;
    char card_line1_[48] = "";
    char card_line2_[48] = "";
};

}  // namespace eidolon
