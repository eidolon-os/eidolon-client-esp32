#pragma once

#include <cstdint>

#include <lvgl.h>

#include "guard/guard_state_machine.h"

class Display;

namespace eidolon {

struct GuardOwnerFaceDisplay {
    bool profile_active = false;
    uint32_t profile_revision = 0;
    uint32_t template_count = 0;
    bool has_result = false;
    uint64_t result_age_ms = 0;
    int faces = 0;
    int id = -1;
    float similarity = 0.0f;
};

// Local-only LVGL surface for an active ATK guard. It owns its display buffer
// and never retains a camera frame pointer.
class GuardDisplay {
public:
    explicit GuardDisplay(Display* display);
    ~GuardDisplay();

    void UpdateObservation(const GuardObservation& observation,
                           const GuardOwnerFaceDisplay& owner_face);
    void Reset();

private:
    Display* display_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* state_label_ = nullptr;
    lv_obj_t* detail_label_ = nullptr;
    lv_obj_t* owner_label_ = nullptr;
    lv_obj_t* owner_detail_label_ = nullptr;
    lv_obj_t* profile_label_ = nullptr;
    lv_obj_t* epoch_label_ = nullptr;
    lv_obj_t* motion_bar_ = nullptr;

    bool EnsurePanel();
    void ApplyState(const GuardObservation& observation,
                    const GuardOwnerFaceDisplay& owner_face);
};

}  // namespace eidolon
