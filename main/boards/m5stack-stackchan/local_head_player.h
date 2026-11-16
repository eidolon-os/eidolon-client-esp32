#pragma once

// Arbitration and stepping for the desk pet's local head clips (layers L2–L4 of
// docs/stackchan-desk-pet-design.md §8), kept apart from FreeRTOS and the servo bus so it
// is host-tested (tests/stackchan_head_player_test.cc). StackChanBody owns one and calls
// it under its motion mutex; it feeds in the clock and the servo angles and writes the
// poses this returns.
//
// What it decides (docs/跨系统/StackChan自然转动分析-20261116.md E1, E3, E5):
//   - Whether a clip may play: never while capture holds the head, a server gesture runs,
//     a look_at hold is armed, within 2 s of a server move, while the servo rail is still
//     coming up, while a clip of higher priority plays, or after a safety.stop until the
//     next server head command or the end of the session.
//   - Where the path starts: the pose last commanded, unless the head is measurably
//     somewhere else (a hand or a server move put it there): then where it really is. A
//     clip that takes over from one still moving starts from that path's pose and speed.
//   - Where the head is at every tick (pet_head_path.h).
//   - Whether the head goes home afterwards: a clip that ends off-centre without `hold`
//     returns home 2.5 s later; a held clip stays.

#include <cmath>
#include <cstdint>

#include "eidolon/pet/pet_head_path.h"

namespace stackchan {

struct HeadPose {
    float yaw = 0, pitch = 0;   // deci-degrees relative to the rest pose
};

class LocalHeadPlayer {
public:
    static constexpr int64_t kServerQuietUs = 2000 * 1000;   // local clips wait after a server move
    static constexpr int64_t kHomeDelayUs = 2500 * 1000;     // a clip left the head off-centre
    // The readback may sit this far from the last command and still count as there: the
    // pitch sags ~6.5 deg below its target under its own weight while it holds (yaw has no
    // load). Starting from a sagged readback would first drop the head further.
    static constexpr float kYawTrustDd = 30, kPitchTrustDd = 90;
    // Return home: as slow as a glance coming back (§4.0 E1, 25 deg/s).
    static constexpr uint16_t kHomeDps = 25;
    static constexpr uint16_t kHomeMinMs = 600;

    // `limits` is kept by reference (one static object per body): it must outlive the player.
    explicit LocalHeadPlayer(const eidolon::pet::HeadPathLimits& limits) : limits_(&limits) {}

    struct Body {
        bool capture_quiet = false;
        bool gesture_busy = false;
        bool look_at_armed = false;
        bool power_ready = true;
    };

    // Why `clip` may not play now; nullptr when it may.
    const char* Refusal(const eidolon::pet::HeadClip& clip, int64_t now_us, const Body& body) const {
        if (clip.count == 0 || clip.count > clip.keys.size()) return "empty";
        if (stop_latched_) return "stopped";
        if (body.capture_quiet) return "capture";
        if (body.gesture_busy) return "gesture";
        if (body.look_at_armed) return "look_at";
        if (server_seen_ && now_us - last_server_us_ < kServerQuietUs) return "server";
        if (!body.power_ready) return "power";
        if (active_ && clip.priority > clip_.priority) return "priority";
        return nullptr;
    }

    // Starts `clip` (already checked with Refusal). `commanded` is where the servos were
    // last told to be, `readback` where they are.
    void Start(const eidolon::pet::HeadClip& clip, int64_t now_us, HeadPose commanded, HeadPose readback) {
        eidolon::pet::HeadPathStart from;
        if (active_) {
            const auto s = Sample(now_us);
            from = {s.yaw, s.pitch, s.vyaw, s.vpitch};
        } else {
            from.yaw = std::fabs(readback.yaw - commanded.yaw) > kYawTrustDd ? readback.yaw : commanded.yaw;
            from.pitch = std::fabs(readback.pitch - commanded.pitch) > kPitchTrustDd ? readback.pitch : commanded.pitch;
        }
        clip_ = clip;
        from_ = from;
        start_us_ = now_us;
        active_ = true;
        home_due_us_ = 0;
    }

    // Starts the return home that Tick asked for.
    void StartHome(int64_t now_us, HeadPose commanded, HeadPose readback) {
        eidolon::pet::HeadClip home;
        home.count = 1;
        home.priority = 4;
        home.hold = true;
        home.keys[0] = eidolon::pet::HeadKey{kHomeMinMs, 0, 0, kHomeDps};
        Start(home, now_us, commanded, readback);
    }

    struct Step {
        bool drive = false;      // write `pose` to the servos
        HeadPose pose;
        bool finished = false;   // the clip just reached its last key
        bool home_due = false;   // read the servos and call StartHome
    };

    Step Tick(int64_t now_us) {
        Step step;
        if (active_) {
            const auto s = Sample(now_us);
            step.drive = true;
            step.pose = {s.yaw, s.pitch};
            if (s.done) {
                active_ = false;
                step.finished = true;
                const auto& last = clip_.keys[clip_.count - 1];
                const bool home = std::abs(last.yaw_dd) <= 20 && std::abs(last.dpitch_dd) <= 40;
                home_due_us_ = clip_.hold || home ? 0 : now_us + kHomeDelayUs;
            }
            return step;
        }
        if (home_due_us_ != 0 && now_us >= home_due_us_) {
            home_due_us_ = 0;
            step.home_due = true;
        }
        return step;
    }

    // Server motion (L1) owns the head now: drop the local clip and any return home. It
    // also ends a safety.stop latch, as does the end of a session (a plain stop).
    void ServerMotion(int64_t now_us) {
        active_ = false;
        home_due_us_ = 0;
        last_server_us_ = now_us;
        server_seen_ = true;
        stop_latched_ = false;
    }
    // safety.stop: as a server motion, and local clips stay off until the next server head
    // command or the end of the session (design §6.2 rule 1).
    void SafetyStop(int64_t now_us) {
        ServerMotion(now_us);
        stop_latched_ = true;
    }
    // Stop where the path is now and stay there (a still is being taken). True when a clip
    // was moving; `pose` is where it stopped.
    bool Hold(int64_t now_us, HeadPose& pose) {
        home_due_us_ = 0;
        if (!active_) return false;
        const auto s = Sample(now_us);
        pose = {s.yaw, s.pitch};
        active_ = false;
        return true;
    }
    // The microphone opened: the body stops the servos itself.
    void Cancel() {
        active_ = false;
        home_due_us_ = 0;
    }

    bool active() const { return active_; }
    bool stop_latched() const { return stop_latched_; }
    bool home_pending() const { return home_due_us_ != 0; }
    // Length of the playing path (ms), for how long the IMU must ignore the head.
    float path_ms() const { return active_ ? eidolon::pet::HeadPathEndMs(clip_, from_, *limits_) : 0.f; }
    const eidolon::pet::HeadPathStart& from() const { return from_; }

private:
    eidolon::pet::HeadSample Sample(int64_t now_us) const {
        return eidolon::pet::SampleHeadPath(clip_, from_, static_cast<float>(now_us - start_us_) / 1000.f, *limits_);
    }

    const eidolon::pet::HeadPathLimits* limits_;
    eidolon::pet::HeadClip clip_{};
    eidolon::pet::HeadPathStart from_{};
    int64_t start_us_ = 0;
    int64_t home_due_us_ = 0;
    int64_t last_server_us_ = 0;
    bool active_ = false;
    bool server_seen_ = false;
    bool stop_latched_ = false;
};

}  // namespace stackchan
