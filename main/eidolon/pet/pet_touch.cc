#include "eidolon/pet/pet_touch.h"

#include <algorithm>
#include <cmath>

namespace eidolon::pet {

namespace {
SenseEvent Event(SenseKind kind, uint64_t t, float value = 0, int dir = 0)
{
    SenseEvent e;
    e.kind = kind;
    e.t_ms = t;
    e.value = value;
    e.dir = static_cast<int8_t>(dir);
    return e;
}
}  // namespace

HeadTouchRecognizer::HeadTouchRecognizer(const HeadTouchConfig& config) : cfg_(config)
{
    cfg_.rapid_taps = std::clamp<uint8_t>(cfg_.rapid_taps, 2, static_cast<uint8_t>(tap_times_.size()));
}

void HeadTouchRecognizer::Update(const HeadTouchSample& s, std::vector<SenseEvent>& out)
{
    last_t_ = s.t_ms;

    // Stuck detection watches the chip, not the hand, so it runs through
    // suppression windows too: the documented stuck state follows a head shake.
    const bool all_full = s.front >= 3 && s.middle >= 3 && s.back >= 3;
    if (!all_full) {
        saturated_ = false;
    } else if (!saturated_) {
        saturated_ = true;
        saturated_since_ = s.t_ms;
    } else if (!needs_recalibration_ && s.t_ms - saturated_since_ > cfg_.stuck_ms) {
        needs_recalibration_ = true;
        // The press was the stuck reading itself; close it so the runtime is
        // not left holding a TouchDown, but classify nothing.
        if (down_) {
            out.push_back(Event(SenseKind::TouchRelease, s.t_ms, position_));
            if (stroking_) {
                out.push_back(Event(SenseKind::TouchStrokeEnd, s.t_ms,
                                    (s.t_ms - press_start_) / 1000.0f, stroke_dir_));
            }
        }
        ResetPress();
        ForgetTaps();
        armed_ = false;
    }

    if (s.t_ms < suppress_until_) {
        ResetPress();
        armed_ = false;
        return;
    }
    if (needs_recalibration_) return;

    if (double_pending_ && s.t_ms >= double_due_) {
        double_pending_ = false;
        out.push_back(Event(SenseKind::TouchDoubleTap, s.t_ms, double_value_));
    }

    const bool pressed = s.front >= 1 || s.middle >= 1 || s.back >= 1;
    if (!armed_) {
        armed_ = !pressed;
        if (armed_) has_release_ = false;  // the dropped press never reported its end
        return;
    }
    if (!pressed) {
        if (down_) EndPress(s.t_ms, out);
        return;
    }
    const float sum = static_cast<float>(s.front + s.middle + s.back);
    const float pos = (static_cast<float>(s.back) - static_cast<float>(s.front)) / sum;
    if (!down_) {
        BeginPress(s, pos, out);
    } else {
        ContinuePress(s, pos, out);
    }
}

void HeadTouchRecognizer::SuppressUntil(uint64_t t_ms)
{
    suppress_until_ = std::max(suppress_until_, t_ms);
    if (suppress_until_ > last_t_) {
        // Drop the gesture now rather than at the next sample: a short window
        // can end before the next sample arrives.
        ResetPress();
        ForgetTaps();
        armed_ = false;
    }
}

void HeadTouchRecognizer::ClearRecalibration()
{
    needs_recalibration_ = false;
    saturated_ = false;
    armed_ = false;
}

void HeadTouchRecognizer::BeginPress(const HeadTouchSample& s, float pos, std::vector<SenseEvent>& out)
{
    double_pending_ = false;  // a third press: the pair was patting, not a double tap
    ResetPress();
    down_ = true;
    press_start_ = s.t_ms;
    position_ = last_pos_ = min_pos_ = max_pos_ = pos;
    pos_sum_ = pos;
    pos_count_ = 1;
    last_motion_ms_ = s.t_ms;
    RecordZones(s);
    press_gap_ = has_release_ ? static_cast<uint32_t>(std::min<uint64_t>(s.t_ms - last_release_, UINT32_MAX - 1))
                              : UINT32_MAX;
    out.push_back(Event(SenseKind::TouchDown, s.t_ms, pos));
}

void HeadTouchRecognizer::ContinuePress(const HeadTouchSample& s, float pos, std::vector<SenseEvent>& out)
{
    const float step = pos - last_pos_;
    if (std::fabs(step) >= cfg_.move_epsilon) {
        motion_dir_ = step > 0 ? 1 : -1;
        last_motion_ms_ = s.t_ms;
    } else if (s.t_ms - last_motion_ms_ > cfg_.move_idle_ms) {
        motion_dir_ = 0;
    }
    last_pos_ = position_ = pos;
    min_pos_ = std::min(min_pos_, pos);
    max_pos_ = std::max(max_pos_, pos);
    pos_sum_ += pos;
    ++pos_count_;
    RecordZones(s);
    out.push_back(Event(SenseKind::TouchMove, s.t_ms, pos, motion_dir_));

    if (!stroking_) CheckStroke(s.t_ms, out);
    if (!stroking_ && !hold_fired_ && s.t_ms - press_start_ >= cfg_.hold_ms &&
        max_pos_ - min_pos_ <= cfg_.still_span) {
        hold_fired_ = true;
        out.push_back(Event(SenseKind::TouchHold, s.t_ms, pos));
    }
}

void HeadTouchRecognizer::EndPress(uint64_t t, std::vector<SenseEvent>& out)
{
    last_press_.gap_ms = press_gap_;
    last_press_.duration_ms = static_cast<uint32_t>(t - press_start_);
    for (int z = 0; z < 3; ++z) last_press_.peak[z] = peak_[z];
    ++presses_;
    out.push_back(Event(SenseKind::TouchRelease, t, position_));
    const bool was_tap = !stroking_ && t - press_start_ < cfg_.tap_max_ms &&
                         max_pos_ - min_pos_ <= cfg_.still_span;
    if (stroking_) {
        // The whole contact counts as stroking: detection can lag the first
        // zone by a sample or two and should not shorten the stroke.
        out.push_back(Event(SenseKind::TouchStrokeEnd, t, (t - press_start_) / 1000.0f, stroke_dir_));
    } else if (was_tap) {
        RegisterTap(t, out);
    }
    if (!was_tap) single_tap_pending_ = false;  // a hold or stroke breaks a double tap
    has_release_ = true;
    last_release_ = t;
    ResetPress();
}

void HeadTouchRecognizer::RecordZones(const HeadTouchSample& s)
{
    const uint8_t level[3] = {s.front, s.middle, s.back};
    for (int z = 0; z < 3; ++z) {
        peak_[z] = std::max(peak_[z], level[z]);
        if (level[z] >= 1 && !zone_seen_[z]) {
            zone_seen_[z] = true;
            zone_first_[z] = s.t_ms;
        }
    }
}

void HeadTouchRecognizer::CheckStroke(uint64_t t, std::vector<SenseEvent>& out)
{
    int dir = 0;
    // Zones lighting up one after another is the clearest stroke. Equal times
    // (a palm landing on two zones in one sample) say nothing about direction.
    if (zone_seen_[kFront] && zone_seen_[kMiddle] && zone_seen_[kBack]) {
        const uint64_t f = zone_first_[kFront], m = zone_first_[kMiddle], b = zone_first_[kBack];
        if (f < m && m < b) dir = +1;
        else if (b < m && m < f) dir = -1;
    }
    // Otherwise one-way travel from the extreme reached so far, so a finger
    // that lands in the middle, drifts forward, then sweeps back still counts.
    if (dir == 0) {
        if (position_ - min_pos_ > cfg_.stroke_travel) dir = +1;
        else if (max_pos_ - position_ > cfg_.stroke_travel) dir = -1;
    }
    if (dir == 0) return;
    stroking_ = true;
    stroke_dir_ = static_cast<int8_t>(dir);
    out.push_back(Event(SenseKind::TouchStrokeStart, t, position_, dir));
}

void HeadTouchRecognizer::RegisterTap(uint64_t t, std::vector<SenseEvent>& out)
{
    const float mean = pos_sum_ / static_cast<float>(pos_count_);
    const bool pairs = single_tap_pending_ && press_start_ - last_tap_release_ < cfg_.double_tap_gap_ms;
    last_tap_release_ = t;

    // A quiet gap longer than the window ends the burst.
    if (tap_count_ > 0 && t - tap_times_[tap_count_ - 1] > cfg_.rapid_window_ms) rapid_latched_ = false;
    uint8_t keep = 0;
    for (uint8_t i = 0; i < tap_count_; ++i) {
        if (t - tap_times_[i] <= cfg_.rapid_window_ms) tap_times_[keep++] = tap_times_[i];
    }
    tap_count_ = keep;
    if (tap_count_ == tap_times_.size()) {
        std::copy(tap_times_.begin() + 1, tap_times_.end(), tap_times_.begin());
        --tap_count_;
    }
    tap_times_[tap_count_++] = t;

    if (rapid_latched_) {   // still patting: already reported
        single_tap_pending_ = false;
        return;
    }
    if (tap_count_ >= cfg_.rapid_taps) {
        rapid_latched_ = true;
        single_tap_pending_ = false;
        out.push_back(Event(SenseKind::TouchRapid, t, static_cast<float>(tap_count_)));
        return;
    }
    if (pairs) {
        double_pending_ = true;
        double_due_ = t + cfg_.double_tap_wait_ms;
        double_value_ = mean;
        single_tap_pending_ = false;  // a third tap starts a new pair
        return;
    }
    SenseKind kind = SenseKind::TouchTapMiddle;
    if (mean < -1.0f / 3) kind = SenseKind::TouchTapFront;
    else if (mean > 1.0f / 3) kind = SenseKind::TouchTapBack;
    out.push_back(Event(kind, t, mean));
    single_tap_pending_ = true;
}

void HeadTouchRecognizer::ResetPress()
{
    down_ = false;
    position_ = last_pos_ = min_pos_ = max_pos_ = 0;
    pos_sum_ = 0;
    pos_count_ = 0;
    motion_dir_ = 0;
    hold_fired_ = false;
    stroking_ = false;
    stroke_dir_ = 0;
    zone_first_.fill(0);
    zone_seen_.fill(false);
    peak_.fill(0);
    press_gap_ = UINT32_MAX;
}

void HeadTouchRecognizer::ForgetTaps()
{
    single_tap_pending_ = false;
    double_pending_ = false;
    tap_count_ = 0;
    rapid_latched_ = false;
}

}  // namespace eidolon::pet
