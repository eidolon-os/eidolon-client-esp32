#include "guard/guard_service.h"

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
        reset_grid_requested_ = true;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        reset_grid_requested_ = true;
        observation = state_machine_.Stop(NowMs());
        should_publish = observation.sequence != published_sequence_;
        published_sequence_ = observation.sequence;
    }
    ESP_LOGI(kTag, "Stop reason=%s", reason != nullptr ? reason : "unspecified");
    if (should_publish) {
        PublishObservation(observation);
    }
    ApplyDisplayState(observation);
}

void GuardService::SetObservationCallback(GuardObservationCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

GuardObservation GuardService::CurrentObservation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_observation_;
}

std::string GuardService::StatusJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& config = state_machine_.config();
    const std::string now_ms = std::to_string(last_observation_.now_ms);
    const std::string last_seen_ms = std::to_string(last_observation_.last_seen_ms);
    char json[512] = {};
    std::snprintf(json, sizeof(json),
                  "{\"running\":%s,\"epoch\":%lu,\"sequence\":%lu,\"state\":\"%s\","
                  "\"motion_score\":%lu,\"ts_ms\":%s,\"last_seen_ms\":%s,"
                  "\"fault\":\"%s\",\"config\":{\"sample_interval_ms\":%lu,"
                  "\"preview_interval_ms\":%lu,"
                  "\"motion_threshold\":%lu,\"motion_clear_threshold\":%lu,"
                  "\"candidate_debounce_ms\":%lu,\"absence_timeout_ms\":%lu,"
                  "\"consecutive_capture_failures\":%lu}}",
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
                  static_cast<unsigned long>(config.consecutive_capture_failures));
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = state_machine_.config();
            running = running_;
            task_exit = task_exit_;
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
        vTaskDelay(pdMS_TO_TICKS(config.sample_interval_ms));
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

    GuardLuminanceGrid current = {};
    bool grid_ready = false;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    OwnerFaceLiveResult owner_face;
#endif
    sample.frame_ok = camera_->AnalyzeFrame([&](const CameraFrame& frame) {
        grid_ready = ReadGuardLuminanceGrid(frame, current);
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        if (grid_ready && owner_face_engine_ != nullptr) {
            owner_face = owner_face_engine_->AnalyzeLiveFrame(frame, now_ms);
        }
#endif
        return grid_ready;
    });
    if (!sample.frame_ok || !grid_ready) {
        return sample;
    }
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    if (owner_face.evaluated) {
        ESP_LOGI(kTag,
                 "owner_face_fact revision=%lu faces=%d id=%d similarity=%.3f",
                 static_cast<unsigned long>(owner_face.profile_revision),
                 owner_face.faces, owner_face.id,
                 static_cast<double>(owner_face.similarity));
    }
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        if (owner_face.evaluated) {
            last_owner_face_result_ = owner_face;
        }
#endif
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
    }
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

void GuardService::ApplyDisplayState(const GuardObservation& observation)
{
    GuardOwnerFaceDisplay owner_face_display;
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    if (owner_face_engine_ != nullptr) {
        const OwnerFaceProfileStatus profile = owner_face_engine_->GetProfileStatus();
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
