#include "guard/guard_service.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display.h"
#include "guard/guard_display.h"
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
#include "guard/owner_face_engine.h"
#endif

namespace eidolon {
namespace {

constexpr char kTag[] = "GuardService";
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
constexpr uint32_t kOwnerAcquireIntervalMs = 200;
#endif
const char* DisplayTextForState(GuardState state, GuardFaultCode fault)
{
    switch (state) {
    case GuardState::Disabled:
        return "Guard disabled";
    case GuardState::Idle:
        return "Guard ready";
    case GuardState::CandidatePending:
        return "Guard watching";
    case GuardState::Candidate:
        return "Guard candidate";
    case GuardState::AbsentPending:
        return "Guard away?";
    case GuardState::Absent:
        return "Guard away";
    case GuardState::Fault:
        return fault == GuardFaultCode::CameraUnavailable ? "Guard camera fault" : "Guard fault";
    }
    return "Guard";
}

}  // namespace

GuardRuntimeConfig GuardService::DefaultConfig()
{
    GuardRuntimeConfig config;
#ifdef CONFIG_EIDOLON_GUARD_SAMPLE_INTERVAL_MS
    config.sample_interval_ms = CONFIG_EIDOLON_GUARD_SAMPLE_INTERVAL_MS;
#endif
#ifdef CONFIG_EIDOLON_GUARD_PREVIEW_INTERVAL_MS
    config.preview_interval_ms = CONFIG_EIDOLON_GUARD_PREVIEW_INTERVAL_MS;
#endif
#ifdef CONFIG_EIDOLON_GUARD_MOTION_THRESHOLD
    config.motion_threshold = CONFIG_EIDOLON_GUARD_MOTION_THRESHOLD;
#endif
#ifdef CONFIG_EIDOLON_GUARD_MOTION_CLEAR_THRESHOLD
    config.motion_clear_threshold = CONFIG_EIDOLON_GUARD_MOTION_CLEAR_THRESHOLD;
#endif
#ifdef CONFIG_EIDOLON_GUARD_CANDIDATE_DEBOUNCE_MS
    config.candidate_debounce_ms = CONFIG_EIDOLON_GUARD_CANDIDATE_DEBOUNCE_MS;
#endif
#ifdef CONFIG_EIDOLON_GUARD_ABSENCE_TIMEOUT_MS
    config.absence_timeout_ms = CONFIG_EIDOLON_GUARD_ABSENCE_TIMEOUT_MS;
#endif
#ifdef CONFIG_EIDOLON_GUARD_CAPTURE_FAILURE_LIMIT
    config.consecutive_capture_failures = CONFIG_EIDOLON_GUARD_CAPTURE_FAILURE_LIMIT;
#endif
    return config;
}

GuardService::GuardService(Camera* camera) : camera_(camera)
{
    state_machine_.ApplyConfig(DefaultConfig());
    last_observation_ = state_machine_.Current(NowMs());
    last_owner_presence_observation_ = owner_presence_state_machine_.Current(NowMs());
    guard_display_ = std::make_unique<GuardDisplay>(Board::GetInstance().GetDisplay());
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    owner_face_engine_ = std::make_unique<OwnerFaceEngine>();
#endif
    if (xTaskCreate(&GuardService::TaskTrampoline, "guard_service", 4096, this, 3, &task_handle_) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create guard service task");
        task_handle_ = nullptr;
    }
}

GuardService::~GuardService()
{
    Stop("destroy");
    std::lock_guard<std::mutex> lock(mutex_);
    task_exit_ = true;
}

bool GuardService::Start(const GuardRuntimeConfig& config, const char* reason)
{
    GuardObservation observation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_machine_.ApplyConfig(config);
        owner_presence_state_machine_.ApplyConfig({
            .enter_ms = config.owner_presence_enter_ms,
            .exit_ms = config.owner_presence_exit_ms,
            .heartbeat_ms = config.owner_presence_heartbeat_ms,
            .lease_ms = config.owner_presence_lease_ms,
        });
        owner_presence_lease_ms_ = config.owner_presence_lease_ms;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        if (owner_face_engine_ != nullptr) {
            owner_face_engine_->SetLiveIntervalMs(
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
                std::min(config.owner_face_interval_ms, kOwnerAcquireIntervalMs)
#else
                config.owner_face_interval_ms
#endif
            );
        }
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        if (person_presence_engine_ == nullptr) {
            person_presence_engine_ = std::make_unique<PersonPresenceEngine>();
        }
        if (person_presence_engine_ != nullptr) {
            person_presence_engine_->SetIntervalMs(kOwnerAcquireIntervalMs);
        }
        last_person_presence_result_ = {};
#endif
#if !CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        reset_grid_requested_ = true;
#endif
        if (camera_ == nullptr) {
            running_ = false;
            observation = state_machine_.Fault(NowMs(), GuardFaultCode::CameraUnavailable);
        } else {
            running_ = true;
            observation = state_machine_.Start(NowMs());
        }
        published_sequence_ = observation.sequence;
    }
    ESP_LOGI(kTag, "Start reason=%s state=%s", reason != nullptr ? reason : "unspecified",
             GuardStateName(observation.state));
    PublishObservation(observation);
    ApplyDisplayState(observation);
    return observation.state != GuardState::Fault;
}

void GuardService::Stop(const char* reason)
{
    GuardObservation observation;
    bool should_publish = false;
    OwnerPresenceObservation owner_presence;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
#if !CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        reset_grid_requested_ = true;
#endif
        observation = state_machine_.Stop(NowMs());
        should_publish = observation.sequence != published_sequence_;
        published_sequence_ = observation.sequence;
        owner_presence = owner_presence_state_machine_.Stop(NowMs());
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        last_person_presence_result_ = {};
#endif
    }
    ESP_LOGI(kTag, "Stop reason=%s", reason != nullptr ? reason : "unspecified");
    if (should_publish) {
        PublishObservation(observation);
    }
    PublishOwnerPresence(owner_presence);
    ApplyDisplayState(observation);
}

void GuardService::SetObservationCallback(GuardObservationCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void GuardService::SetOwnerPresenceCallback(OwnerPresenceCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    owner_presence_callback_ = std::move(callback);
}

GuardObservation GuardService::CurrentObservation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_observation_;
}

OwnerPresenceObservation GuardService::CurrentOwnerPresence() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return owner_presence_state_machine_.Current(NowMs());
}

uint32_t GuardService::OwnerPresenceLeaseMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return owner_presence_lease_ms_;
}

std::string GuardService::StatusJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& config = state_machine_.config();
    const std::string now_ms = std::to_string(last_observation_.now_ms);
    const std::string last_seen_ms = std::to_string(last_observation_.last_seen_ms);
    char json[768] = {};
    std::snprintf(json, sizeof(json),
                  "{\"running\":%s,\"epoch\":%lu,\"sequence\":%lu,\"state\":\"%s\","
                  "\"motion_score\":%lu,\"ts_ms\":%s,\"last_seen_ms\":%s,"
                  "\"fault\":\"%s\",\"config\":{\"sample_interval_ms\":%lu,"
                  "\"preview_interval_ms\":%lu,"
                  "\"motion_threshold\":%lu,\"motion_clear_threshold\":%lu,"
                  "\"candidate_debounce_ms\":%lu,\"absence_timeout_ms\":%lu,"
                  "\"consecutive_capture_failures\":%lu},"
                  "\"owner_presence\":{\"state\":\"%s\",\"profile_revision\":%lu,"
                  "\"epoch\":%lu,\"sequence\":%lu,\"last_match_ms\":%llu}}",
                  running_ ? "true" : "false",
                  static_cast<unsigned long>(last_observation_.epoch),
                  static_cast<unsigned long>(last_observation_.sequence),
                  GuardStateName(last_observation_.state),
                  static_cast<unsigned long>(last_observation_.motion_score),
                  now_ms.c_str(), last_seen_ms.c_str(),
                  GuardFaultCodeName(last_observation_.fault),
                  static_cast<unsigned long>(config.sample_interval_ms),
                  static_cast<unsigned long>(config.preview_interval_ms),
                  static_cast<unsigned long>(config.motion_threshold),
                  static_cast<unsigned long>(config.motion_clear_threshold),
                  static_cast<unsigned long>(config.candidate_debounce_ms),
                  static_cast<unsigned long>(config.absence_timeout_ms),
                  static_cast<unsigned long>(config.consecutive_capture_failures),
                  OwnerPresenceStateName(last_owner_presence_observation_.state),
                  static_cast<unsigned long>(last_owner_presence_observation_.profile_revision),
                  static_cast<unsigned long>(last_owner_presence_observation_.epoch),
                  static_cast<unsigned long>(last_owner_presence_observation_.sequence),
                  static_cast<unsigned long long>(last_owner_presence_observation_.last_match_ms));
    return json;
}

bool GuardService::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void GuardService::TaskTrampoline(void* arg)
{
    static_cast<GuardService*>(arg)->TaskLoop();
}

void GuardService::TaskLoop()
{
    while (true) {
        GuardRuntimeConfig config;
        bool running = false;
        bool task_exit = false;
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        bool owner_profile_active = false;
#endif
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = state_machine_.config();
            running = running_;
            task_exit = task_exit_;
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
            owner_profile_active = last_owner_face_profile_status_.active;
#endif
        }
        if (task_exit) {
            vTaskDelete(nullptr);
            return;
        }
        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const uint64_t now_ms = NowMs();
        const GuardSample sample = CaptureSample(now_ms);
        GuardObservation observation;
        bool should_publish = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            observation = state_machine_.ProcessSample(sample);
            should_publish = observation.sequence != published_sequence_;
            if (should_publish) {
                published_sequence_ = observation.sequence;
                if (observation.state == GuardState::Fault) {
                    running_ = false;
                }
            }
        }
        if (should_publish) {
            PublishObservation(observation);
        }
        ApplyDisplayState(observation);
        uint32_t loop_interval_ms = config.sample_interval_ms;
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        if (owner_profile_active) {
            loop_interval_ms = std::min(loop_interval_ms, kOwnerAcquireIntervalMs);
        }
#endif
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(loop_interval_ms));
    }
}

GuardSample GuardService::CaptureSample(uint64_t now_ms)
{
    GuardSample sample = {
        .now_ms = now_ms,
        .frame_ok = false,
        .motion_valid = false,
        .motion_score = 0,
    };
    if (camera_ == nullptr) {
        return sample;
    }

#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
    uint8_t frame_probe = 0;
#else
    GuardLuminanceGrid current = {};
#endif
    bool frame_ready = false;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    OwnerFaceLiveResult owner_face;
    OwnerFaceProfileStatus owner_profile;
    bool owner_profile_updated = false;
    OwnerPresenceObservation owner_presence;
    bool run_owner_face = true;
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
    PersonPresenceResult person_presence;
    bool run_person_presence = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_person_presence =
            last_owner_presence_observation_.identity_session_active;
        run_owner_face = !run_person_presence;
    }
#endif
    sample.frame_ok = camera_->AnalyzeFrame([&](const CameraFrame& frame) {
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        frame_ready = ReadGuardLuminanceImage(frame, &frame_probe, 1, 1, false);
#else
        frame_ready = ReadGuardLuminanceGrid(frame, current);
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        if (frame_ready && run_person_presence &&
            person_presence_engine_ != nullptr) {
            person_presence = person_presence_engine_->AnalyzeFrame(frame, now_ms);
        }
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        if (frame_ready && run_owner_face && owner_face_engine_ != nullptr) {
            owner_face =
                owner_face_engine_->AnalyzeLiveFrame(frame, now_ms);
        }
#endif
        return frame_ready;
    });
    if (!sample.frame_ok || !frame_ready) {
        return sample;
    }
    // Inference is synchronous and can take hundreds of milliseconds. State
    // transitions and deadlines must use completion time rather than the stale
    // timestamp captured before the camera/inference call.
    now_ms = NowMs();
    sample.now_ms = now_ms;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    if (owner_face.evaluated) {
        owner_face.evaluated_at_ms = now_ms;
    }
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
    if (person_presence.evaluated) {
        person_presence.evaluated_at_ms = now_ms;
    }
#endif
    if (owner_face.evaluated && owner_face.faces > 0) {
        ESP_LOGI(kTag,
                 "owner_face_fact revision=%lu faces=%d id=%d similarity=%.3f",
                 static_cast<unsigned long>(owner_face.profile_revision),
                 owner_face.faces, owner_face.id,
                 static_cast<double>(owner_face.similarity));
    }
    if (owner_face_engine_ != nullptr) {
        owner_profile_updated = owner_face_engine_->TryGetProfileStatus(owner_profile);
    }
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        if (owner_profile_updated) {
            last_owner_face_profile_status_ = owner_profile;
        } else {
            owner_profile = last_owner_face_profile_status_;
        }
        if (owner_face.evaluated) {
            last_owner_face_result_ = owner_face;
        }
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        if (person_presence.evaluated) {
            last_person_presence_result_ = person_presence;
        }
#endif
        owner_presence = owner_presence_state_machine_.Process({
            .now_ms = now_ms,
            .profile_active = owner_profile.active,
            .profile_revision = owner_profile.revision,
            .face_evaluated = owner_face.evaluated,
            .face_match = owner_face.evaluated && owner_face.id >= 0,
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
            .person_evaluated = person_presence.evaluated,
            .person_present = person_presence.present,
#endif
        });
        last_owner_presence_observation_ = owner_presence;
#endif
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        sample.motion_valid = true;
#else
        if (reset_grid_requested_) {
            has_previous_grid_ = false;
            reset_grid_requested_ = false;
        }
        if (has_previous_grid_) {
            sample.motion_score = GuardMotionScore(previous_grid_, current);
            sample.motion_valid = true;
        } else {
            sample.motion_score = 0;
            sample.motion_valid = true;
            has_previous_grid_ = true;
        }
        previous_grid_ = current;
#endif
    }
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    PublishOwnerPresence(owner_presence);
#endif
    return sample;
}

void GuardService::PublishObservation(const GuardObservation& observation)
{
    GuardObservationCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_observation_ = observation;
        callback = callback_;
    }

    const std::string now_ms = std::to_string(observation.now_ms);
    const std::string last_seen_ms = std::to_string(observation.last_seen_ms);

    ESP_LOGI(kTag,
             "observation epoch=%lu seq=%lu state=%s score=%lu ts=%s last_seen=%s fault=%s",
             static_cast<unsigned long>(observation.epoch),
             static_cast<unsigned long>(observation.sequence),
             GuardStateName(observation.state),
             static_cast<unsigned long>(observation.motion_score),
             now_ms.c_str(), last_seen_ms.c_str(),
             GuardFaultCodeName(observation.fault));
    if (callback) {
        callback(observation);
    }
}

void GuardService::PublishOwnerPresence(const OwnerPresenceObservation& observation)
{
    if (observation.fact == OwnerPresenceFact::None) {
        return;
    }
    OwnerPresenceCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_owner_presence_observation_ = observation;
        callback = owner_presence_callback_;
    }
    ESP_LOGI(kTag,
             "owner_presence state=%s fact=%s revision=%lu epoch=%lu sequence=%lu",
             OwnerPresenceStateName(observation.state),
             OwnerPresenceFactName(observation.fact),
             static_cast<unsigned long>(observation.fact_profile_revision),
             static_cast<unsigned long>(observation.epoch),
             static_cast<unsigned long>(observation.sequence));
    if (callback) {
        callback(observation);
    }
}

void GuardService::ApplyDisplayState(const GuardObservation& observation)
{
    GuardOwnerFaceDisplay owner_face_display;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    {
        std::lock_guard<std::mutex> lock(mutex_);
        owner_face_display.presence_state = last_owner_presence_observation_.state;
        owner_face_display.identity_session_active =
            last_owner_presence_observation_.identity_session_active;
#if CONFIG_EIDOLON_OWNER_PERSON_PRESENCE
        owner_face_display.person_evaluated =
            last_person_presence_result_.evaluated;
        owner_face_display.person_present = last_person_presence_result_.present;
        owner_face_display.person_score = last_person_presence_result_.person_score;
#endif
    }
    if (owner_face_engine_ != nullptr) {
        OwnerFaceProfileStatus profile;
        const bool profile_status_updated =
            owner_face_engine_->TryGetProfileStatus(profile);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (profile_status_updated) {
                last_owner_face_profile_status_ = profile;
            } else {
                profile = last_owner_face_profile_status_;
            }
        }
        owner_face_display.profile_active = profile.active;
        owner_face_display.profile_revision = profile.revision;
        owner_face_display.template_count = profile.template_count;
        if (profile.active) {
            OwnerFaceLiveResult last_result;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_result = last_owner_face_result_;
            }
            if (last_result.evaluated && last_result.profile_revision == profile.revision) {
                const uint64_t now_ms = NowMs();
                owner_face_display.has_result = true;
                owner_face_display.result_age_ms =
                    now_ms >= last_result.evaluated_at_ms
                        ? now_ms - last_result.evaluated_at_ms
                        : 0;
                owner_face_display.faces = last_result.faces;
                owner_face_display.id = last_result.id;
                owner_face_display.similarity = last_result.similarity;
            }
        }
    }
#endif
    if (guard_display_ != nullptr) {
        guard_display_->UpdateObservation(observation, owner_face_display);
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->SetStatus(DisplayTextForState(observation.state, observation.fault));
}

uint64_t GuardService::NowMs() const
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace eidolon
