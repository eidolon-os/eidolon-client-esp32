#pragma once

#include <cstdint>

#include <lvgl.h>

#include "guard/guard_state_machine.h"

class Display;

namespace eidolon {

// Local-only LVGL surface for an active ATK guard. It owns its display buffer
// and never retains a camera frame pointer.
class GuardDisplay {
public:
    explicit GuardDisplay(Display* display);
    ~GuardDisplay();

    void UpdateObservation(const GuardObservation& observation);
    void Reset();

private:
    Display* display_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* state_label_ = nullptr;
    lv_obj_t* detail_label_ = nullptr;
    lv_obj_t* epoch_label_ = nullptr;
    lv_obj_t* motion_bar_ = nullptr;

    bool EnsurePanel();
    void ApplyState(const GuardObservation& observation);
};

}  // namespace eidolon
