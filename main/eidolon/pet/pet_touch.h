#pragma once

// Head-top touch gestures from the Si12T three-zone pad.
// Pure logic: fed ~40 Hz samples by the board driver, host-testable.

#include <array>
#include <cstdint>
#include <vector>

#include "eidolon/pet/pet_sense.h"

namespace eidolon::pet {

// Initial values from the design notes and the factory firmware; every one of
// them still has to be confirmed on a real StackChan head.
struct HeadTouchConfig {
    // A still press that ends before it becomes a hold is a tap (a pat).
    // 11-16: pats while patting rapidly lasted up to 776 ms, a slow single tap
    // 380 ms; at the old 300 ms they got no reaction and did not count.
    uint32_t tap_max_ms = 800;
    float still_span = 0.5f;          // tap/hold: max spread of position in one press
    // Previous tap's release -> this press. 11-16 on the head: deliberate
    // double taps 76-384 ms apart, separate single taps never under 840 ms.
    uint32_t double_tap_gap_ms = 450;
    // How long a double tap waits for a third press before it is reported: a
    // third makes it patting (pats were 75-220 ms apart). Shorter than the gap
    // so the info card is not held back longer than it must be.
    uint32_t double_tap_wait_ms = 320;
    uint32_t rapid_window_ms = 2000;
    uint8_t rapid_taps = 4;           // clamped to 2..8
    uint32_t hold_ms = 800;
    float stroke_travel = 0.7f;       // one-way travel (position units) = stroke
    float move_epsilon = 0.15f;       // smaller steps are quantisation noise
    uint32_t move_idle_ms = 300;      // motion direction decays to 0 after this
    // All zones at 3 this long = chip stuck. Long enough that a palm resting on the
    // head (the "hold" reaction) is never mistaken for it and recalibrated away.
    uint32_t stuck_ms = 20000;
};

// One finished press as the pad saw it (diagnostics).
struct TouchPressSummary {
    uint32_t gap_ms = UINT32_MAX;   // since the previous release; UINT32_MAX if none
    uint32_t duration_ms = 0;
    uint8_t peak[3] = {};           // highest level per zone: front, middle, back
};

// Events per press, in order:
//   TouchDown (first pressed sample), TouchMove (every later pressed sample),
//   TouchHold / TouchStrokeStart (at most once each, while held),
//   TouchRelease, then TouchStrokeEnd if the press stroked, otherwise
//   TouchTap* if it was a tap.
// Taps in quick succession:
//   a second tap within double_tap_gap_ms of the first is held back for
//   double_tap_wait_ms; TouchDoubleTap if no third press starts by then,
//   otherwise it is patting and the pair reports nothing more.
//   rapid_taps taps within rapid_window_ms: TouchRapid once, and the rest of
//   the burst reports no taps (the pet is already annoyed).
class HeadTouchRecognizer {
public:
    explicit HeadTouchRecognizer(const HeadTouchConfig& config = HeadTouchConfig());

    void Update(const HeadTouchSample& sample, std::vector<SenseEvent>& out);

    // Servo motion couples into the electrodes. Until t_ms nothing is
    // reported, the gesture in progress is dropped silently (no Release), and
    // afterwards a new press needs a clean release first, so a reading left
    // over from the motion cannot start one.
    void SuppressUntil(uint64_t t_ms);

    // All three zones pinned at 3 for longer than stuck_ms: the chip's
    // reference is off (seen for ~2 min after a head shake). Touch is ignored
    // until ClearRecalibration(), and then until the pad reads released.
    bool NeedsRecalibration() const { return needs_recalibration_; }
    void ClearRecalibration();

    bool down() const { return down_; }
    float position() const { return down_ ? position_ : 0.0f; }  // -1 front .. +1 back
    bool stroking() const { return stroking_; }
    int direction() const { return down_ ? motion_dir_ : 0; }    // recent motion, as TouchMove.dir
    uint32_t presses() const { return presses_; }   // finished presses so far
    const TouchPressSummary& last_press() const { return last_press_; }

private:
    enum Zone { kFront = 0, kMiddle = 1, kBack = 2 };

    void BeginPress(const HeadTouchSample& s, float pos, std::vector<SenseEvent>& out);
    void ContinuePress(const HeadTouchSample& s, float pos, std::vector<SenseEvent>& out);
    void EndPress(uint64_t t, std::vector<SenseEvent>& out);
    void RecordZones(const HeadTouchSample& s);
    void CheckStroke(uint64_t t, std::vector<SenseEvent>& out);
    void RegisterTap(uint64_t t, std::vector<SenseEvent>& out);
    void ResetPress();
    void ForgetTaps();

    HeadTouchConfig cfg_;

    uint64_t last_t_ = 0;
    uint64_t suppress_until_ = 0;
    bool armed_ = true;  // false: wait for an all-released sample before a new press
    bool has_release_ = false;     // last_release_ is a reported press's end
    uint64_t last_release_ = 0;
    uint32_t presses_ = 0;
    TouchPressSummary last_press_;

    // Stuck detection.
    bool saturated_ = false;
    uint64_t saturated_since_ = 0;
    bool needs_recalibration_ = false;

    // Current press.
    bool down_ = false;
    uint64_t press_start_ = 0;
    float position_ = 0;
    float last_pos_ = 0;
    float min_pos_ = 0, max_pos_ = 0;
    float pos_sum_ = 0;
    uint32_t pos_count_ = 0;
    int8_t motion_dir_ = 0;
    uint64_t last_motion_ms_ = 0;
    bool hold_fired_ = false;
    bool stroking_ = false;
    int8_t stroke_dir_ = 0;
    std::array<uint64_t, 3> zone_first_{};  // first touch time per zone in this press
    std::array<bool, 3> zone_seen_{};
    std::array<uint8_t, 3> peak_{};
    uint32_t press_gap_ = UINT32_MAX;

    // Tap history.
    bool single_tap_pending_ = false;  // last tap could pair into a double tap
    bool double_pending_ = false;      // a double tap waiting to see no third press
    uint64_t double_due_ = 0;
    float double_value_ = 0;
    uint64_t last_tap_release_ = 0;
    std::array<uint64_t, 8> tap_times_{};
    uint8_t tap_count_ = 0;            // valid entries in tap_times_, oldest first
    bool rapid_latched_ = false;       // one TouchRapid per burst
};

}  // namespace eidolon::pet
