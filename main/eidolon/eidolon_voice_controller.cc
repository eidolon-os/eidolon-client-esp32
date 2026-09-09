#include "eidolon_voice_controller.h"

#include "controller_worker_resources.h"

#include "board.h"
#include "control_protocol.h"
#include "device_event_builder.h"
#include "device_identity.h"
#include "eidolon_topics.h"
#include "eidolon_local_feedback.h"
#if CONFIG_EIDOLON_GUARD_SERVICE
#include "guard/guard_service.h"
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
#include "guard/owner_face_engine.h"
#endif
#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
#include "guard/vision_benchmark.h"
#endif
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "hub_onboarding_client.h"
#include "hub_onboarding_protocol.h"
#include "hub_trust_store.h"
#include "authority_locator.h"
#include "livekit_board.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <new>
#include <stdio.h>

#ifndef CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS
#define CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS 0
#endif

#ifndef CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS
#define CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS 0
#endif

#ifndef CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS
#define CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS 0
#endif

#define TAG "EidolonVoice"

namespace {
std::string OperationalDeviceInstanceId()
{
    auto& identity = eidolon::DeviceIdentity::GetInstance();
    return identity.EnsureKeypair() == ESP_OK
        ? identity.DeviceInstanceId()
        : std::string{};
}

constexpr int64_t kPlaybackActiveWindowUs = 1200 * 1000;
// Poll the audio state fast so a barge-in (near-end speech) edge reaches the
// channel within ~one poll, but only emit an unchanged heartbeat every
// kAudioStateHeartbeatUs to avoid flooding lossy packets at the poll rate.
constexpr uint64_t kAudioTickIntervalUs = 80 * 1000;
constexpr int64_t kAudioStateHeartbeatUs = 500 * 1000;
constexpr uint32_t kRadarPresenceLeaseMs = 15000;
constexpr uint32_t kRadarPresenceHeartbeatMs = 5000;
constexpr uint32_t kRadarStatePublishRetryMs = 1000;
constexpr time_t kValidUnixTimeFloor = 1'700'000'000;
// A connect/reconnect attempt that never reaches a terminal LiveKit state within
// this long is treated as hung and force-recovered. Generous enough to cover a
// slow mDNS + HTTPS + connect on a healthy-but-slow network.
constexpr uint64_t kConnectWatchdogUs = 25ULL * 1000 * 1000;
// PTT: Channel owns half-duplex idle teardown and sends session_end before
// deleting the voice room. This client timer is a lifecycle fallback only, so a
// missed data packet / room-close callback cannot leave the UI stuck in a voice
// room. Default 0 disables it.
constexpr uint64_t kPttIdleFallbackUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS) * 1000ULL;
// PTT release tail: keep capture open briefly after touch release before
// publishing ptt=false. This avoids clipping the final phoneme while preserving
// an explicit, device-owned turn boundary.
constexpr uint64_t kPttReleaseTailUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS) * 1000ULL;
// Full-duplex: Channel owns idle teardown and sends session_end before deleting
// the voice room. This client timer is a lifecycle fallback only, so a missed
// data packet / room-close callback cannot leave the UI stuck in "listening".
constexpr uint64_t kFullDuplexIdleFallbackUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS) * 1000ULL;
// Single controller task: drains the event queue, serializing all state mutation.
constexpr UBaseType_t kControllerTaskPriority = 5;
constexpr UBaseType_t kEventQueueLen = 24;
// Let the control-room ack flush before switching to the voice room.
constexpr TickType_t kRoomJoinSettleDelay = pdMS_TO_TICKS(500);
// Let the "succeeded" ack flush before reconnecting the channel.
constexpr TickType_t kActiveAckSettleDelay = pdMS_TO_TICKS(100);
#if CONFIG_EIDOLON_GUARD_SERVICE
constexpr size_t kMaxPendingGuardPresenceEvents = 8;
#endif

const char* LiveKitConnectionStateName(eidolon::LiveKitConnectionState state)
{
    switch (state) {
    case eidolon::LiveKitConnectionState::Disconnected:
        return "Disconnected";
    case eidolon::LiveKitConnectionState::Connecting:
        return "Connecting";
    case eidolon::LiveKitConnectionState::Connected:
        return "Connected";
    case eidolon::LiveKitConnectionState::Reconnecting:
        return "Reconnecting";
    case eidolon::LiveKitConnectionState::Failed:
        return "Failed";
    }
    return "unknown";
}

const char* AgentPhaseName(eidolon::AgentPhase phase)
{
    switch (phase) {
    case eidolon::AgentPhase::Silent:
        return "silent";
    case eidolon::AgentPhase::UserSpeaking:
        return "user_speaking";
    case eidolon::AgentPhase::AgentThinking:
        return "agent_thinking";
    case eidolon::AgentPhase::AgentSpeaking:
        return "agent_speaking";
    }
    return "unknown";
}

#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
bool ParseAmbientPresenceState(const std::string& payload,
                               eidolon::AmbientPresenceState* result)
{
    if (result == nullptr) {
        return false;
    }
    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(
        payload.data(), payload.size(), &parse_end, false);
    if (root == nullptr || parse_end != payload.data() + payload.size() ||
        !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 6) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON* modality = cJSON_GetObjectItemCaseSensitive(root, "modality");
    const cJSON* epoch =
        cJSON_GetObjectItemCaseSensitive(root, "presence_epoch");
    const cJSON* sequence =
        cJSON_GetObjectItemCaseSensitive(root, "sequence");
    const cJSON* lease = cJSON_GetObjectItemCaseSensitive(root, "lease_ms");
    const cJSON* observation =
        cJSON_GetObjectItemCaseSensitive(root, "observation");
    const bool present = cJSON_IsString(state) &&
                         strcmp(state->valuestring, "present") == 0;
    const bool vacant = cJSON_IsString(state) &&
                        strcmp(state->valuestring, "vacant") == 0;
    eidolon::AmbientPresenceObservation parsed_observation =
        eidolon::AmbientPresenceObservation::Edge;
    bool observation_valid = cJSON_IsString(observation);
    if (observation_valid &&
        strcmp(observation->valuestring, "snapshot") == 0) {
        parsed_observation =
            eidolon::AmbientPresenceObservation::Snapshot;
    } else if (observation_valid &&
               strcmp(observation->valuestring, "heartbeat") == 0) {
        parsed_observation =
            eidolon::AmbientPresenceObservation::Heartbeat;
    } else if (!observation_valid ||
               strcmp(observation->valuestring, "edge") != 0) {
        observation_valid = false;
    }
    const bool valid =
        (present || vacant) &&
        cJSON_IsString(modality) &&
        strcmp(modality->valuestring, "mmwave") == 0 &&
        cJSON_IsNumber(epoch) && epoch->valuedouble >= 1 &&
        epoch->valuedouble <= 4294967295.0 &&
        std::floor(epoch->valuedouble) == epoch->valuedouble &&
        cJSON_IsNumber(sequence) && sequence->valuedouble >= 1 &&
        sequence->valuedouble <= 4294967295.0 &&
        std::floor(sequence->valuedouble) == sequence->valuedouble &&
        cJSON_IsNumber(lease) && lease->valuedouble >= 0 &&
        lease->valuedouble <= 60000 &&
        std::floor(lease->valuedouble) == lease->valuedouble &&
        ((present && lease->valuedouble > 0) ||
         (vacant && lease->valuedouble == 0)) &&
        !(vacant &&
          parsed_observation ==
              eidolon::AmbientPresenceObservation::Heartbeat) &&
        observation_valid;
    if (valid) {
        *result = {
            .present = present,
            .presence_epoch = static_cast<uint32_t>(epoch->valuedouble),
            .sequence = static_cast<uint32_t>(sequence->valuedouble),
            .lease_ms = static_cast<uint32_t>(lease->valuedouble),
            .observation = parsed_observation,
        };
    }
    cJSON_Delete(root);
    return valid;
}
#endif

#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
bool ParseOwnerConfirmationLease(
    const std::string& payload, std::string* ambient_source_device_id,
    uint32_t* ambient_presence_epoch, uint32_t* lease_ms,
    uint32_t* guard_epoch, uint32_t* presence_sequence)
{
    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(
        payload.data(), payload.size(), &parse_end, false);
    if (root == nullptr || parse_end != payload.data() + payload.size() ||
        !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 8 ||
        ambient_source_device_id == nullptr ||
        ambient_presence_epoch == nullptr || lease_ms == nullptr ||
        guard_epoch == nullptr ||
        presence_sequence == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    int profile_count = 0;
    int ambient_source_count = 0;
    int ambient_epoch_count = 0;
    int epoch_count = 0;
    int sequence_count = 0;
    int evidence_count = 0;
    int retention_count = 0;
    int lease_count = 0;
    bool only_known_fields = true;
    for (const cJSON* item = root->child; item != nullptr; item = item->next) {
        const char* key = item->string ? item->string : "";
        if (strcmp(key, "ambient_source_device_id") == 0) {
            ++ambient_source_count;
        } else if (strcmp(key, "ambient_presence_epoch") == 0) {
            ++ambient_epoch_count;
        } else if (strcmp(key, "profile_revision") == 0) {
            ++profile_count;
        } else if (strcmp(key, "guard_epoch") == 0) {
            ++epoch_count;
        } else if (strcmp(key, "presence_sequence") == 0) {
            ++sequence_count;
        } else if (strcmp(key, "evidence") == 0) {
            ++evidence_count;
        } else if (strcmp(key, "raw_retention") == 0) {
            ++retention_count;
        } else if (strcmp(key, "lease_ms") == 0) {
            ++lease_count;
        } else {
            only_known_fields = false;
        }
    }
    const cJSON* ambient_source =
        cJSON_GetObjectItemCaseSensitive(root, "ambient_source_device_id");
    const cJSON* ambient_epoch =
        cJSON_GetObjectItemCaseSensitive(root, "ambient_presence_epoch");
    const cJSON* profile =
        cJSON_GetObjectItemCaseSensitive(root, "profile_revision");
    const cJSON* epoch =
        cJSON_GetObjectItemCaseSensitive(root, "guard_epoch");
    const cJSON* sequence =
        cJSON_GetObjectItemCaseSensitive(root, "presence_sequence");
    const cJSON* evidence =
        cJSON_GetObjectItemCaseSensitive(root, "evidence");
    const cJSON* retention =
        cJSON_GetObjectItemCaseSensitive(root, "raw_retention");
    const cJSON* lease = cJSON_GetObjectItemCaseSensitive(root, "lease_ms");
    const bool valid =
        only_known_fields && ambient_source_count == 1 &&
        ambient_epoch_count == 1 && profile_count == 1 && epoch_count == 1 &&
        sequence_count == 1 && evidence_count == 1 && retention_count == 1 &&
        lease_count == 1 && cJSON_IsString(ambient_source) &&
        ambient_source->valuestring[0] != '\0' &&
        strlen(ambient_source->valuestring) <= 128 &&
        cJSON_IsNumber(ambient_epoch) &&
        ambient_epoch->valuedouble >= 1 &&
        ambient_epoch->valuedouble <= 4294967295.0 &&
        std::floor(ambient_epoch->valuedouble) ==
            ambient_epoch->valuedouble &&
        cJSON_IsNumber(profile) && profile->valuedouble >= 1 &&
        std::floor(profile->valuedouble) == profile->valuedouble &&
        cJSON_IsNumber(epoch) && epoch->valuedouble >= 0 &&
        epoch->valuedouble <= 4294967295.0 &&
        std::floor(epoch->valuedouble) == epoch->valuedouble &&
        cJSON_IsNumber(sequence) && sequence->valuedouble >= 1 &&
        sequence->valuedouble <= 4294967295.0 &&
        std::floor(sequence->valuedouble) == sequence->valuedouble &&
        cJSON_IsString(evidence) &&
        strcmp(evidence->valuestring, "local_owner_face") == 0 &&
        cJSON_IsString(retention) &&
        strcmp(retention->valuestring, "none") == 0 &&
        cJSON_IsNumber(lease) && lease->valuedouble >= 1 &&
        lease->valuedouble <= 120000 &&
        std::floor(lease->valuedouble) == lease->valuedouble;
    if (valid) {
        *ambient_source_device_id = ambient_source->valuestring;
        *ambient_presence_epoch =
            static_cast<uint32_t>(ambient_epoch->valuedouble);
        *lease_ms = static_cast<uint32_t>(lease->valuedouble);
        *guard_epoch = static_cast<uint32_t>(epoch->valuedouble);
        *presence_sequence = static_cast<uint32_t>(sequence->valuedouble);
    }
    cJSON_Delete(root);
    return valid;
}

bool ParseOwnerPresenceChanged(const std::string& payload, bool* present,
                               uint32_t* lease_ms, uint32_t* guard_epoch,
                               uint32_t* presence_sequence)
{
    if (present == nullptr || lease_ms == nullptr || guard_epoch == nullptr ||
        presence_sequence == nullptr) {
        return false;
    }
    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(
        payload.data(), payload.size(), &parse_end, false);
    if (root == nullptr || parse_end != payload.data() + payload.size() ||
        !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 7) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON* profile = cJSON_GetObjectItemCaseSensitive(root, "profile_revision");
    const cJSON* epoch = cJSON_GetObjectItemCaseSensitive(root, "guard_epoch");
    const cJSON* sequence = cJSON_GetObjectItemCaseSensitive(root, "presence_sequence");
    const cJSON* lease = cJSON_GetObjectItemCaseSensitive(root, "lease_ms");
    const cJSON* evidence = cJSON_GetObjectItemCaseSensitive(root, "evidence");
    const cJSON* retention = cJSON_GetObjectItemCaseSensitive(root, "raw_retention");
    const bool is_present = cJSON_IsString(state) &&
                            strcmp(state->valuestring, "present") == 0;
    const bool is_absent = cJSON_IsString(state) &&
                           strcmp(state->valuestring, "absent") == 0;
    const bool valid =
        (is_present || is_absent) &&
        cJSON_IsNumber(profile) && profile->valuedouble >= 1 &&
        cJSON_IsNumber(epoch) && epoch->valuedouble >= 0 &&
        epoch->valuedouble <= 4294967295.0 &&
        cJSON_IsNumber(sequence) && sequence->valuedouble >= 1 &&
        sequence->valuedouble <= 4294967295.0 &&
        cJSON_IsNumber(lease) && lease->valuedouble >= 0 &&
        lease->valuedouble <= 120000 &&
        std::floor(profile->valuedouble) == profile->valuedouble &&
        std::floor(epoch->valuedouble) == epoch->valuedouble &&
        std::floor(sequence->valuedouble) == sequence->valuedouble &&
        std::floor(lease->valuedouble) == lease->valuedouble &&
        ((is_present && lease->valuedouble > 0) ||
         (is_absent && lease->valuedouble == 0)) &&
        cJSON_IsString(evidence) &&
        strcmp(evidence->valuestring, "face_gated_person_presence") == 0 &&
        cJSON_IsString(retention) &&
        strcmp(retention->valuestring, "none") == 0;
    if (valid) {
        *present = is_present;
        *lease_ms = static_cast<uint32_t>(lease->valuedouble);
        *guard_epoch = static_cast<uint32_t>(epoch->valuedouble);
        *presence_sequence = static_cast<uint32_t>(sequence->valuedouble);
    }
    cJSON_Delete(root);
    return valid;
}
#endif

bool ProviderBindingExpired(const eidolon::Esp32HubConfig& config)
{
    if (config.expires_at_ms <= 0) {
        return true;
    }
    const time_t now = std::time(nullptr);
    if (now < kValidUnixTimeFloor) {
        // TLS/SNTP initialization owns wall-clock establishment. Until then,
        // attempt an online refresh first but do not reject a bounded cached
        // credential solely because the local clock still reads the epoch.
        return false;
    }
    return static_cast<int64_t>(now) * 1000 >= config.expires_at_ms;
}
}  // namespace

namespace eidolon {

// ============================ Event loop plumbing ============================

EidolonVoiceController::EidolonVoiceController(GuardService* guard_service)
    : guard_service_(guard_service)
{
    event_queue_ = xQueueCreate(kEventQueueLen, sizeof(Event));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create controller event queue");
        return;
    }
    if (xTaskCreate(&EidolonVoiceController::TaskTrampoline, "eidolon_ctrl",
                    kControllerWorkerStackBytes, this, kControllerTaskPriority,
                    &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create controller task");
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST || \
    CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    esp_timer_create_args_t presence_timer_args = {};
    presence_timer_args.callback =
        &EidolonVoiceController::AmbientPresenceTimerCb;
    presence_timer_args.arg = this;
    presence_timer_args.dispatch_method = ESP_TIMER_TASK;
    presence_timer_args.name = "ambient_presence";
    if (esp_timer_create(&presence_timer_args,
                         &ambient_presence_timer_) != ESP_OK) {
        ambient_presence_timer_ = nullptr;
    }
#endif
#if CONFIG_EIDOLON_GUARD_SERVICE && CONFIG_EIDOLON_OWNER_FACE_PROFILE && \
    CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    if (guard_service_ != nullptr) {
        RegisterDeviceEventHandler(
            kAmbientPresenceStateType,
            [this](const DeviceEventMessage& event) {
                HandleAmbientPresenceEvent(event);
            });
    }
#endif
#if CONFIG_BOARD_TYPE_ESP_BOX_3
    RegisterDeviceEventHandler(
        kIdentityOwnerPresenceConfirmedType,
        [this](const DeviceEventMessage& event) {
            HandleOwnerPresenceConfirmedEvent(event);
        });
    RegisterDeviceEventHandler(
        kIdentityOwnerPresenceChangedType,
        [this](const DeviceEventMessage& event) {
            HandleOwnerPresenceChangedEvent(event);
        });
#endif
}

EidolonVoiceController::~EidolonVoiceController()
{
    // Best-effort teardown; in practice the controller lives for the app lifetime.
    for (esp_timer_handle_t* t :
         {&audio_timer_, &reconnect_timer_, &connect_watchdog_, &idle_leave_timer_,
          &full_duplex_idle_timer_, &ptt_release_tail_timer_,
          &ambient_presence_timer_, &onboarding_poll_timer_}) {
        if (*t != nullptr) {
            esp_timer_stop(*t);
            esp_timer_delete(*t);
            *t = nullptr;
        }
    }
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (event_queue_ != nullptr) {
        Event ev;
        while (xQueueReceive(event_queue_, &ev, 0) == pdTRUE) {
            delete ev.payload;
            if (ev.completion != nullptr) {
                (*ev.completion)(false);
                delete ev.completion;
            }
        }
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
}

void EidolonVoiceController::TaskTrampoline(void* arg)
{
    static_cast<EidolonVoiceController*>(arg)->ControllerLoop();
    vTaskDelete(nullptr);
}

void EidolonVoiceController::ControllerLoop()
{
    Event ev;
    for (;;) {
        if (xQueueReceive(event_queue_, &ev, portMAX_DELAY) == pdTRUE) {
            Dispatch(ev);
            delete ev.payload;
            delete ev.completion;
        }
    }
}

void EidolonVoiceController::Enqueue(Event ev)
{
    if (event_queue_ == nullptr) {
        delete ev.payload;
        if (ev.completion != nullptr) {
            (*ev.completion)(false);
            delete ev.completion;
        }
        return;
    }
    // The queue copies the struct (including the payload pointer); on success the
    // loop owns and frees it, on failure we free it here.
    if (xQueueSend(event_queue_, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full; dropped event type=%d", static_cast<int>(ev.type));
        delete ev.payload;
        if (ev.completion != nullptr) {
            (*ev.completion)(false);
            delete ev.completion;
        }
    }
}

void EidolonVoiceController::Dispatch(const Event& ev)
{
    switch (ev.type) {
    case EventType::Activation:
        DoActivation();
        break;
    case EventType::NetworkLost:
        DoNetworkLost();
        break;
    case EventType::NetworkRestored:
        DoNetworkRestored();
        break;
    case EventType::CommissioningQuiesce:
        if (ev.completion != nullptr) {
            (*ev.completion)(DoCommissioningQuiesce());
        }
        break;
    case EventType::Join:
        DoJoinRoom();
        break;
    case EventType::Leave:
        DoLeaveRoom();
        break;
    case EventType::SetMic:
        DoSetMicEnabled(ev.flag);
        break;
    case EventType::PttPress:
        DoPttPressed();
        break;
    case EventType::PttRelease:
        DoPttReleased();
        break;
    case EventType::PttReleaseTail:
        DoPttReleaseTail();
        break;
    case EventType::LiveKitState:
        DoLiveKitState(ev.lk_state, ev.generation);
        break;
#if CONFIG_EIDOLON_GUARD_SERVICE
    case EventType::GuardObservation:
        DoGuardObservation(ev.guard_observation, ev.generation);
        break;
    case EventType::OwnerPresence:
        DoOwnerPresence(ev.owner_presence_observation, ev.generation);
        break;
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    case EventType::OwnerFaceProfileCompleted:
        if (ev.payload != nullptr) {
            DoOwnerFaceProfileCompleted(*ev.payload);
        }
        break;
#endif
    case EventType::ControlCommand:
        if (ev.payload != nullptr) {
            DoControlCommand(*ev.payload);
        }
        break;
    case EventType::SessionControl:
        if (ev.payload != nullptr) {
            DoSessionControl(*ev.payload);
        }
        break;
    case EventType::DeviceEvent:
        if (ev.payload != nullptr) {
            DoDeviceEvent(*ev.payload, ev.generation);
        }
        break;
    case EventType::PublishDeviceEvent:
        if (ev.payload != nullptr) {
            DoPublishDeviceEvent(*ev.payload);
        }
        break;
    case EventType::AmbientPresence:
        DoAmbientPresenceChanged(ev.flag);
        break;
    case EventType::AmbientPresenceTimer:
        DoAmbientPresenceTimer();
        break;
    case EventType::AgentPhaseChanged:
        DoAgentPhase(ev.phase);
        break;
    case EventType::SessionActivity:
        DoSessionActivity();
        break;
    case EventType::AudioTick:
        DoAudioTick();
        break;
    case EventType::OnboardingPoll:
        DoOnboardingPoll();
        break;
    case EventType::ReconnectTick:
        DoReconnectTick();
        break;
    case EventType::ConnectTimeout:
        DoConnectTimeout();
        break;
    case EventType::IdleLeave:
        DoIdleAutoLeave();
        break;
    case EventType::FullDuplexIdleFallback:
        DoFullDuplexIdleFallback();
        break;
    }
}

// ============================ Public entry points ============================
// All just post an event; the work happens on the controller task. Returns are
// ignored by callers (LiveKitVoiceTransport), so success/failure is reported via
// state transitions, not the synchronous return.

void EidolonVoiceController::OnHubActivationSucceeded()
{
    Event ev;
    ev.type = EventType::Activation;
    Enqueue(ev);
}

void EidolonVoiceController::OnNetworkLost()
{
    ESP_LOGW(TAG, "[lifecycle] enqueue network_lost state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::NetworkLost;
    Enqueue(ev);
}

void EidolonVoiceController::OnNetworkRestored()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue network_restored state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::NetworkRestored;
    Enqueue(ev);
}

void EidolonVoiceController::QuiesceForCommissioning(
    std::function<void(bool)> completion)
{
    auto* owned = new (std::nothrow) std::function<void(bool)>;
    if (owned == nullptr) {
        completion(false);
        return;
    }
    *owned = std::move(completion);
    Event ev;
    ev.type = EventType::CommissioningQuiesce;
    ev.completion = owned;
    Enqueue(ev);
}

void EidolonVoiceController::OnAmbientPresenceChanged(bool present)
{
    Event ev;
    ev.type = EventType::AmbientPresence;
    ev.flag = present;
    Enqueue(ev);
}

esp_err_t EidolonVoiceController::JoinRoom()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue join state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::Join;
    Enqueue(ev);
    return ESP_OK;
}

esp_err_t EidolonVoiceController::LeaveRoom()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue leave state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::Leave;
    Enqueue(ev);
    return ESP_OK;
}

esp_err_t EidolonVoiceController::SetMicEnabled(bool enabled)
{
    Event ev;
    ev.type = EventType::SetMic;
    ev.flag = enabled;
    Enqueue(ev);
    return ESP_OK;
}

bool EidolonVoiceController::RegisterDeviceEventHandler(
    const std::string& type, DeviceEventBus::Handler handler)
{
    return device_event_bus_.RegisterHandler(type, std::move(handler));
}

esp_err_t EidolonVoiceController::PublishDeviceEvent(const std::string& payload)
{
    if (payload.empty() || payload.size() > DeviceEventBus::kMaxEventBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    Event ev;
    ev.type = EventType::PublishDeviceEvent;
    ev.payload = new (std::nothrow) std::string(payload);
    if (ev.payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    Enqueue(ev);
    return ESP_OK;
}

void EidolonVoiceController::OnPttPressed()
{
    ESP_LOGI(TAG, "[ptt] enqueue press state=%s room_kind=%s gen=%lu ptt_active=%d tail=%d",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0);
    Event ev;
    ev.type = EventType::PttPress;
    Enqueue(ev);
}

void EidolonVoiceController::OnPttReleased()
{
    ESP_LOGI(TAG, "[ptt] enqueue release state=%s room_kind=%s gen=%lu ptt_active=%d tail=%d",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0);
    Event ev;
    ev.type = EventType::PttRelease;
    Enqueue(ev);
}

void EidolonVoiceController::SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb)
{
    on_transcription_ = std::move(cb);
    session_.SetOnTranscription([this](const TranscriptionEvent& event) {
        if (!event.text.empty()) {
            Event ev;
            ev.type = EventType::SessionActivity;
            Enqueue(ev);
        }
        if (on_transcription_) {
            on_transcription_(event);
        }
    });
}

void EidolonVoiceController::SetOnAgentPhase(std::function<void(AgentPhase)> cb)
{
    on_agent_phase_ = std::move(cb);
    session_.SetOnAgentPhase([this](AgentPhase phase) {
        Event ev;
        ev.type = EventType::AgentPhaseChanged;
        ev.phase = phase;
        Enqueue(ev);
    });
}

// ============================ Config / state helpers ============================

VoiceSessionState EidolonVoiceController::StateForConfig(const Esp32HubConfig& config) const
{
    switch (config.status) {
    case HubConfigStatus::PendingApproval:
        return VoiceSessionState::PendingApproval;
    case HubConfigStatus::WaitingBinding:
        return VoiceSessionState::WaitingBinding;
    case HubConfigStatus::Active:
        return VoiceSessionState::ConfigReady;
    case HubConfigStatus::RecoveryRequired:
    case HubConfigStatus::Revoked:
        return VoiceSessionState::Unauthorized;
    }
    return VoiceSessionState::Error;
}

bool EidolonVoiceController::HasActiveConfig() const
{
#if CONFIG_EIDOLON_GUARD_SERVICE
    // Guard registration receives a stable data-only channel in the
    // legacy `active` slot.  It must never be treated as a normal voice room.
    return false;
#else
    return config_.status == HubConfigStatus::Active && config_.session.usable() &&
           !ProviderBindingExpired(config_);
#endif
}

bool EidolonVoiceController::HasChannelConfig() const
{
    if (config_.status != HubConfigStatus::Active) {
        return false;
    }
    return config_.session.usable() && !config_.session.room_name.empty() &&
           !ProviderBindingExpired(config_);
}

const char* EidolonVoiceController::VoiceStateName(VoiceSessionState state)
{
    switch (state) {
    case VoiceSessionState::Idle:
        return "Idle";
    case VoiceSessionState::PendingApproval:
        return "PendingApproval";
    case VoiceSessionState::WaitingBinding:
        return "WaitingBinding";
    case VoiceSessionState::ConfigReady:
        return "ConfigReady";
    case VoiceSessionState::Connecting:
        return "Connecting";
    case VoiceSessionState::Opening:
        return "Opening";
    case VoiceSessionState::InRoom:
        return "InRoom";
    case VoiceSessionState::Reconnecting:
        return "Reconnecting";
    case VoiceSessionState::Error:
        return "Error";
    case VoiceSessionState::Unauthorized:
        return "Unauthorized";
    case VoiceSessionState::ServerUnreachable:
        return "ServerUnreachable";
    }
    return "Unknown";
}

const char* EidolonVoiceController::CurrentRoomKind() const
{
    if (standby_) {
        return "control";
    }
    if (session_.IsConnected() || state_ == VoiceSessionState::Connecting ||
        state_ == VoiceSessionState::Opening ||
        state_ == VoiceSessionState::InRoom || state_ == VoiceSessionState::Reconnecting) {
        return "voice";
    }
    return "none";
}

uint32_t EidolonVoiceController::BeginSessionGeneration(const char* room_kind)
{
    session_generation_ += 1;
    ESP_LOGI(TAG, "[lifecycle] begin gen=%lu room_kind=%s (state=%s)",
             static_cast<unsigned long>(session_generation_), room_kind,
             VoiceStateName(state_));
    return session_generation_;
}

void EidolonVoiceController::MarkSessionSuperseded(const char* reason)
{
    session_generation_ += 1;
    ESP_LOGI(TAG, "[lifecycle] superseded gen=%lu reason=%s (state=%s)",
             static_cast<unsigned long>(session_generation_), reason ? reason : "unspecified",
             VoiceStateName(state_));
}

void EidolonVoiceController::SetState(VoiceSessionState state, const char* reason)
{
    if (state_ == state) {
        return;
    }
    VoiceSessionState prev = state_;
    state_ = state;
    // [lifecycle] is the grep anchor for aligning client transitions with the
    // channel's room-lifecycle logs by room_name + timestamp (plan Phase 0).
    ESP_LOGI(TAG, "[lifecycle] SetState %s -> %s reason=%s room_kind=%s gen=%lu",
             VoiceStateName(prev), VoiceStateName(state), reason ? reason : "unspecified",
             CurrentRoomKind(), static_cast<unsigned long>(session_generation_));
    // The watchdog runs only while an attempt is in flight; any terminal state
    // (InRoom / ConfigReady / Error / ...) disarms it.
    if (state == VoiceSessionState::Connecting || state == VoiceSessionState::Opening ||
        state == VoiceSessionState::Reconnecting) {
        ArmConnectWatchdog();
    } else {
        DisarmConnectWatchdog();
    }
    UpdateIdleAutoLeave();  // arms in-room/idle, disarms on leaving the room
    ResetFullDuplexIdleFallback("state_changed");
    if (on_state_changed_) {
        on_state_changed_(state);
    }
}

void EidolonVoiceController::SetOperationalReady(bool ready, const char* reason)
{
    if (operational_ready_ == ready) {
        return;
    }
    operational_ready_ = ready;
    ESP_LOGI(TAG, "[lifecycle] operational_ready=%d reason=%s room_kind=%s gen=%lu",
             ready ? 1 : 0, reason ? reason : "unspecified", CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    if (on_operational_ready_) {
        on_operational_ready_(ready);
    }
}

esp_err_t EidolonVoiceController::LoadStoredConfig()
{
    HubConfigStore store;
    if (!store.Load(config_) || LoadAuthorityRoutes() != ESP_OK) {
        ESP_LOGE(TAG, "No valid Hub config in NVS");
        SetState(VoiceSessionState::Error, "no_stored_config");
        return ESP_ERR_NOT_FOUND;
    }
    SetState(StateForConfig(config_), "config_loaded");
    return ESP_OK;
}

esp_err_t EidolonVoiceController::LoadAuthorityRoutes()
{
    auto& locator = DeviceAuthorityLocator::GetInstance();
    if (locator.ReloadCommissionedDirectory() != ESP_OK) {
        device_control_uri_.clear();
        return ESP_ERR_NOT_ALLOWED;
    }
    device_foundation::v1::AuthorityEndpoint endpoint;
    const esp_err_t err = locator.Resolve(
        device_foundation::v1::LogicalAuthority::DeviceControl, endpoint);
    if (err != ESP_OK) {
        device_control_uri_.clear();
        return err;
    }
    device_control_uri_ = endpoint.uri;
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RefreshHubConfig(bool persist)
{
    if (device_control_uri_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    HubOnboardingClient client;
    Esp32HubConfig fresh;
    esp_err_t err = client.Resume(OperationalDeviceInstanceId(), fresh);
    if (err == ESP_ERR_NOT_ALLOWED && fresh.status == HubConfigStatus::RecoveryRequired) {
        config_ = {};
        config_.status = HubConfigStatus::RecoveryRequired;
        config_.recovery_hint = fresh.recovery_hint;
        DoNetworkLost();
        SetState(VoiceSessionState::Unauthorized, "owner_recovery_required");
        return err;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        // Two different facts arrive as this one code. The Owner removing their
        // device is a decision about this device, and the way back is setup.
        // A 401/403 is one rejected request against a key the Authority did not
        // accept, and the way back is re-approval. Telling a person the wrong
        // one sends them to the wrong place.
        if (fresh.status == HubConfigStatus::Revoked) {
            ESP_LOGW(TAG, "Owner removed this device; setup is required to claim it again");
            SetState(VoiceSessionState::Unauthorized, "claim_revoked");
            return err;
        }
        // Stop bouncing on the same rejected key; show "awaiting re-approval"
        // (admin must re-approve / the device must re-enroll). Recovers on a
        // later successful fetch or reboot.
        ESP_LOGW(TAG, "Hub rejected device identity; awaiting re-approval");
        SetState(VoiceSessionState::Unauthorized, "hub_rejected_identity");
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (persist) {
        // Provider credentials may rotate on every approved handoff. The
        // per-JOIN refresh keeps fresh credentials in RAM to avoid NVS wear.
        HubConfigStore store;
        err = store.SaveHubConfig(fresh);
        if (err != ESP_OK) {
            return err;
        }
    }
    config_ = std::move(fresh);
    SetState(StateForConfig(config_), "config_refreshed");
    if (config_.status == HubConfigStatus::PendingApproval ||
        config_.status == HubConfigStatus::WaitingBinding) {
        ScheduleOnboardingPoll();
    } else if (onboarding_poll_timer_ != nullptr) {
        esp_timer_stop(onboarding_poll_timer_);
    }
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RediscoverHub()
{
    HubDiscovery discovery;
    AuthorityCandidateRecord txt;
    esp_err_t err = discovery.Discover(txt);
    if (err != ESP_OK || txt.owner_domain_descriptor_uri.empty()) {
        ESP_LOGW(TAG, "Hub rediscovery failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    HubOnboardingClient client;
    Esp32HubConfig fresh;
    err = client.Run(txt, OperationalDeviceInstanceId(), fresh);
    if (err != ESP_OK) {
        return err;
    }
    err = LoadAuthorityRoutes();
    if (err != ESP_OK) return err;
    config_ = std::move(fresh);
    HubConfigStore store;
    if (store.SaveHubConfig(config_) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist rediscovered Hub config");
    }
    SetState(StateForConfig(config_), "hub_rediscovered");
    if (config_.status == HubConfigStatus::PendingApproval ||
        config_.status == HubConfigStatus::WaitingBinding) {
        ScheduleOnboardingPoll();
    }
    return ESP_OK;
}

// ============================ LiveKit state handler ============================

void EidolonVoiceController::DoLiveKitState(LiveKitConnectionState lk_state,
                                           uint32_t event_generation)
{
    const char* lk_name = LiveKitConnectionStateName(lk_state);
    bool stale = event_generation != session_generation_;
    // [lifecycle] full attribution of every LiveKit event: which plane the
    // controller thinks it is on, the generation the event was emitted under vs
    // the current one (mismatch == a superseded connection's late event leaking
    // in — the cause of the JOIN bounce), the last SDK failure reason, and the
    // room names involved so this aligns with the channel logs.
    ESP_LOGI(TAG,
             "[lifecycle] lk_event=%s room_kind=%s event_gen=%lu cur_gen=%lu%s "
             "state=%s switching=%d failure=%s room=%s",
             lk_name, standby_ ? "control" : "voice",
             static_cast<unsigned long>(event_generation),
             static_cast<unsigned long>(session_generation_), stale ? " STALE" : "",
             VoiceStateName(state_), switching_to_voice_ ? 1 : 0,
             livekit_failure_reason_str(session_.LastFailureReason()),
             config_.session.room_name.c_str());

    // Generation gate (plan §3.2): every connect (voice or control) bumps
    // session_generation_; the callback snapshots the generation it fired under.
    // An event whose generation is not the current one belongs to a superseded
    // connection — e.g. an old standby attempt's late Disconnected/Failed during a
    // JOIN handoff, or a hung attempt we already abandoned. Dropping it here is
    // what stops the InRoom→Ready bounce; it replaces the old single-shot
    // expect_control_teardown_ heuristic, which only masked exactly one
    // Disconnected and missed Failed / reordered / multi-event teardowns.
    if (stale) {
        ESP_LOGI(TAG, "[lifecycle] dropping stale lk_event=%s (event_gen=%lu cur_gen=%lu)",
                 lk_name, static_cast<unsigned long>(event_generation),
                 static_cast<unsigned long>(session_generation_));
        return;
    }

    if (standby_) {
        CloseConversationAudio();
        switch (lk_state) {
        case LiveKitConnectionState::Connecting:
            SetOperationalReady(false, "channel_connecting");
            channel_recovery_.OnConnecting();
            ArmConnectWatchdog();
            break;
        case LiveKitConnectionState::Connected:
            channel_recovery_.OnConnected();
            SetOperationalReady(true, "channel_connected");
            if (reconnect_timer_ != nullptr) {
                esp_timer_stop(reconnect_timer_);
            }
            DisarmConnectWatchdog();
#if CONFIG_EIDOLON_GUARD_SERVICE
            FlushPendingGuardPresence();
#endif
            if (current_conversation_id_.empty()) {
                SetState(StateForConfig(config_), "channel_connected");
            } else {
                // A confirmed conversation is durable desired state across a
                // transport reconnect. Re-announce it idempotently, then
                // restore local media without waiting for a second start ack.
                // An unconfirmed open may have reached a dispatch whose ack was
                // lost with the old transport, so supersede it with a fresh id.
                if (!conversation_confirmed_) {
                    current_conversation_id_ = NewConversationId();
                }
                const esp_err_t open_err = PublishSessionRequest(
                    kSessionOpenType, current_conversation_id_);
                if (open_err == ESP_OK) {
                    if (conversation_confirmed_) {
                        standby_ = false;
                        channel_recovery_.OnConversationStarted();
                        SetState(VoiceSessionState::InRoom,
                                 "channel_reconnected_conversation_restored");
                        OpenConversationAudio();
                    } else {
                        SetState(VoiceSessionState::Opening,
                                 "channel_connected_session_open_sent");
                    }
                } else {
                    current_conversation_id_.clear();
                    conversation_confirmed_ = false;
                    last_end_reason_ = EndReason::Error;
                    SetState(VoiceSessionState::Error,
                             "channel_connected_session_open_failed");
                }
            }
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
            if (radar_presence_known_) {
                PublishRadarPresenceState(
                    radar_presence_dirty_
                        ? radar_pending_observation_
                        : AmbientPresenceObservation::Snapshot);
            }
#endif
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
            ScheduleAmbientPresenceLeaseExpiry(
                static_cast<uint64_t>(esp_timer_get_time() / 1000));
#endif
            break;
        case LiveKitConnectionState::Failed:
            channel_recovery_.OnDisconnected();
            SetOperationalReady(false, "channel_failed");
            DisarmConnectWatchdog();
            ESP_LOGW(TAG, "Channel connection failed");
            if (!current_conversation_id_.empty()) {
                SetState(VoiceSessionState::Reconnecting,
                         "channel_failed_during_conversation_open");
            }
            ScheduleChannelReconnect("channel_failed");
            break;
        case LiveKitConnectionState::Disconnected:
            channel_recovery_.OnDisconnected();
            SetOperationalReady(false, "channel_disconnected");
            DisarmConnectWatchdog();
            if (!current_conversation_id_.empty()) {
                SetState(VoiceSessionState::Reconnecting,
                         "channel_disconnected_during_conversation_open");
            }
            ScheduleChannelReconnect("channel_disconnected");
            break;
        case LiveKitConnectionState::Reconnecting:
            // Let the SDK repair a transient ICE/DTLS interruption, but bound the
            // attempt: if it never reaches Connected/Failed/Disconnected, the
            // same watchdog forces the ordinary recovery loop.
            SetOperationalReady(false, "channel_reconnecting");
            channel_recovery_.OnReconnecting();
            if (!current_conversation_id_.empty()) {
                SetState(VoiceSessionState::Reconnecting,
                         "channel_reconnecting_during_conversation");
            } else {
                ArmConnectWatchdog();
            }
            break;
        }
        return;
    }

    switch (lk_state) {
    case LiveKitConnectionState::Connecting:
        SetOperationalReady(false, "voice_connecting");
        SetState(VoiceSessionState::Connecting, "voice_connecting");
        break;
    case LiveKitConnectionState::Connected:
        channel_recovery_.OnConversationStarted();
        SetOperationalReady(true, "voice_connected");
        SetState(VoiceSessionState::InRoom, "voice_connected");
        SetPresenceWakePhase(PresenceWakePhase::Idle);
        StartAudioStatePublisher();
        if (pending_room_join_command_active_ &&
            (pending_room_join_generation_ == 0 ||
             pending_room_join_generation_ == event_generation)) {
            char result[192];
            snprintf(result, sizeof(result),
                     "{\"status\":\"in_room\",\"room_name\":\"%s\",\"generation\":%lu}",
                     config_.session.room_name.c_str(),
                     static_cast<unsigned long>(event_generation));
            CompletePendingRoomJoinCommand("completed", "OK", "", result);
        }
        break;
    case LiveKitConnectionState::Reconnecting:
        SetOperationalReady(false, "voice_reconnecting");
        SetState(VoiceSessionState::Reconnecting, "voice_reconnecting");
        break;
    case LiveKitConnectionState::Failed:
        SetOperationalReady(false, "voice_failed");
        SetPresenceWakePhase(PresenceWakePhase::Idle);
        CloseConversationAudio();
        agent_phase_ = AgentPhase::Silent;
        CompletePendingRoomJoinCommand(
            "failed", "ROOM_JOIN_FAILED",
            livekit_failure_reason_str(session_.LastFailureReason()));
        standby_ = true;
        if (!current_conversation_id_.empty()) {
            SetState(VoiceSessionState::Reconnecting,
                     "channel_failed_during_conversation");
            ScheduleChannelReconnect("channel_failed_during_conversation");
            break;
        }
        if (session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_DELETED ||
            session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_CLOSED) {
            // [lifecycle] Room Deleted/Closed: the server tore down the voice room.
            // Today the client cannot tell a normal end (idle/proactive done) from a
            // failed join — both land here. Phase 1 distinguishes them via the
            // session_end{reason} packet the channel now sends before deleting.
            ESP_LOGI(TAG,
                     "[lifecycle] voice room closed by server (failure=%s) room=%s; "
                     "returning to standby",
                     livekit_failure_reason_str(session_.LastFailureReason()),
                     config_.session.room_name.c_str());
            SetState(StateForConfig(config_), "voice_room_closed");
            ScheduleChannelReconnect("voice_room_closed");
        } else {
            SetState(VoiceSessionState::Error, "voice_failed");
            ScheduleChannelReconnect("voice_failed");
        }
        break;
    case LiveKitConnectionState::Disconnected:
        SetOperationalReady(false, "voice_disconnected");
        SetPresenceWakePhase(PresenceWakePhase::Idle);
        // A control-room teardown during a JOIN handoff is now dropped by the
        // generation gate above (it carries the pre-bump generation), so anything
        // reaching here under the current generation is a genuine voice-room drop.
        CloseConversationAudio();
        agent_phase_ = AgentPhase::Silent;
        CompletePendingRoomJoinCommand("failed", "ROOM_JOIN_DISCONNECTED");
        standby_ = true;
        if (!current_conversation_id_.empty()) {
            SetState(VoiceSessionState::Reconnecting,
                     "channel_disconnected_during_conversation");
        } else if (state_ != VoiceSessionState::Idle &&
                   state_ != VoiceSessionState::ConfigReady &&
                   state_ != VoiceSessionState::PendingApproval &&
                   state_ != VoiceSessionState::WaitingBinding) {
            SetState(StateForConfig(config_), "channel_disconnected");
        }
        ScheduleChannelReconnect("voice_disconnected");
        break;
    }
}

// ============================ Reconnect / watchdog / idle ============================

void EidolonVoiceController::ScheduleOnboardingPoll()
{
    if (onboarding_poll_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::OnboardingPollTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "hub_onboarding";
        if (esp_timer_create(&args, &onboarding_poll_timer_) != ESP_OK) {
            onboarding_poll_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create onboarding poll timer");
            return;
        }
    }
    esp_timer_stop(onboarding_poll_timer_);
    if (esp_timer_start_once(onboarding_poll_timer_, 5000000ULL) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to schedule onboarding poll");
    }
}

void EidolonVoiceController::OnboardingPollTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::OnboardingPoll;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoOnboardingPoll()
{
    if (state_ != VoiceSessionState::PendingApproval &&
        state_ != VoiceSessionState::WaitingBinding) {
        return;
    }
    esp_err_t err = RefreshHubConfig();
    if (err == ESP_ERR_NOT_FOUND) {
        err = RediscoverHub();
    }
    if (err != ESP_OK || config_.status != HubConfigStatus::Active) {
        ESP_LOGD(TAG, "Onboarding still pending: %s", esp_err_to_name(err));
        ScheduleOnboardingPoll();
        return;
    }
    ESP_LOGI(TAG, "Administrator approval complete; provider binding is ready");
    if (HasChannelConfig()) {
        ConnectChannel();
    }
}

void EidolonVoiceController::ScheduleChannelReconnect(const char* reason)
{
    // A timer cannot free internal RAM. Once LiveKitSession has measured the
    // same internal-memory shortfall attempt after attempt, another tick is not
    // a retry — it is the same measurement again, and the device would spend the
    // rest of its uptime taking it every 30 s while telling nobody why. Say it
    // once, stop the loop, and wait for an edge that could actually change the
    // answer (network restored, re-activation, a person asking for a
    // conversation), each of which clears the ceiling explicitly.
    if (session_.InternalMemoryCeilingReached()) {
        if (!memory_ceiling_announced_) {
            memory_ceiling_announced_ = true;
            SetState(VoiceSessionState::Error, "channel_internal_memory_exhausted");
            ESP_LOGE(TAG,
                     "[lifecycle] channel reconnect abandoned reason=%s cause=internal_memory "
                     "verdict=%s short_by=%u bytes — this will not clear on its own; see the "
                     "[mem] ledger above for what is holding internal RAM",
                     reason ? reason : "disconnect",
                     session_.InternalMemoryVerdictName(),
                     static_cast<unsigned>(session_.InternalMemoryShortfallBytes()));
        }
        return;
    }
    if (!channel_recovery_.TrySchedule(switching_to_voice_, HasChannelConfig())) {
        return;
    }
    const int scheduled_attempt = channel_recovery_.reconnect_attempts();
    const uint32_t delay_ms = ChannelReconnectDelayMs(scheduled_attempt);
    ESP_LOGI(TAG,
             "[lifecycle] channel reconnect scheduled reason=%s attempt=%d delay_ms=%lu gen=%lu",
             reason ? reason : "disconnect", scheduled_attempt,
             static_cast<unsigned long>(delay_ms),
             static_cast<unsigned long>(session_generation_));

    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::ReconnectTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            channel_recovery_.OnScheduleFailed();
            ESP_LOGW(TAG, "Failed to create reconnect timer");
            return;
        }
    }
    esp_timer_stop(reconnect_timer_);
    const uint64_t delay_us = static_cast<uint64_t>(delay_ms) * 1000ULL;
    if (esp_timer_start_once(reconnect_timer_, delay_us) != ESP_OK) {
        channel_recovery_.OnScheduleFailed();
        ESP_LOGW(TAG, "Failed to start reconnect timer");
    }
}

void EidolonVoiceController::ReconnectTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::ReconnectTick;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoReconnectTick()
{
    int attempt = 0;
    if (!channel_recovery_.BeginRetry(&attempt)) {
        return;
    }
    ESP_LOGI(TAG, "[lifecycle] channel reconnect tick attempt=%d gen=%lu",
             attempt, static_cast<unsigned long>(session_generation_));
    if (ShouldRediscoverChannelConfig(attempt)) {
        // The Hub address may have changed; re-query mDNS and re-fetch config
        // before reconnecting. Authorization/config-status changes terminate
        // this recovery owner instead of reviving stale cached credentials.
        const esp_err_t refresh_err = RediscoverHub();
        if (refresh_err == ESP_ERR_NOT_ALLOWED || !HasChannelConfig()) {
            ESP_LOGW(TAG, "Control recovery stopped after credential refresh: %s",
                     esp_err_to_name(refresh_err));
            channel_recovery_.FinishRetry();
            return;
        }
    }
    if (ShouldEnterStandbyServerUnreachable(
            attempt, !current_conversation_id_.empty())) {
        // Keep a durable conversation in its reconnecting lifecycle. The
        // standby-only ServerUnreachable state must not turn a recoverable
        // conversation into a terminal failure merely because the retry count
        // crossed a presentation threshold.
        SetState(VoiceSessionState::ServerUnreachable, "reconnect_exhausted");
    }

    // Keep the pending guard up across the synchronous connection call. A sync
    // failure is rescheduled below; async terminal events arrive after it clears.
    esp_err_t err = ConnectChannel();
    channel_recovery_.FinishRetry();
    if (err != ESP_OK) {
        ScheduleChannelReconnect("channel_retry");
    }
}

void EidolonVoiceController::ArmConnectWatchdog()
{
    if (connect_watchdog_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::ConnectWatchdogCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_conn_wd";
        if (esp_timer_create(&args, &connect_watchdog_) != ESP_OK) {
            connect_watchdog_ = nullptr;
            return;
        }
    }
    esp_timer_stop(connect_watchdog_);  // restart the window for this attempt
    esp_timer_start_once(connect_watchdog_, kConnectWatchdogUs);
}

void EidolonVoiceController::DisarmConnectWatchdog()
{
    if (connect_watchdog_ != nullptr) {
        esp_timer_stop(connect_watchdog_);
    }
}

void EidolonVoiceController::ConnectWatchdogCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::ConnectTimeout;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoConnectTimeout()
{
    // The attempt may have completed between the timer firing and now.
    const bool voice_connecting =
        state_ == VoiceSessionState::Connecting || state_ == VoiceSessionState::Opening ||
        state_ == VoiceSessionState::Reconnecting;
    const bool standby_connecting = standby_ && channel_recovery_.connect_in_flight();
    if (!voice_connecting && !standby_connecting) {
        return;
    }
    ESP_LOGW(TAG,
             "[lifecycle] connect watchdog fired state=%s room_kind=%s gen=%lu; forcing recovery",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    CloseConversationAudio();
    CompletePendingRoomJoinCommand("failed", "ROOM_JOIN_TIMEOUT");
    if (state_ == VoiceSessionState::Opening && session_.IsConnected()) {
        const std::string expired_conversation_id = current_conversation_id_;
        current_conversation_id_.clear();
        conversation_confirmed_ = false;
        if (!expired_conversation_id.empty()) {
            PublishSessionRequest(kSessionCloseType, expired_conversation_id);
        }
        last_end_reason_ = EndReason::Error;
        standby_ = true;
        SetState(StateForConfig(config_), "session_start_timeout");
        return;
    }
    // Drop the in-flight guards so ScheduleChannelReconnect isn't suppressed, then
    // tear down the hung session and fall back to a stable base state.
    switching_to_voice_ = false;
    channel_recovery_.OnDisconnected();
    channel_recovery_.FinishRetry();
    session_.Disconnect();
    standby_ = true;
    current_conversation_id_.clear();
    conversation_confirmed_ = false;
    last_end_reason_ = EndReason::Error;
    // Supersede the hung attempt so any of its late callbacks are dropped rather
    // than accepted after we fall back (the next ConnectChannel bumps again).
    MarkSessionSuperseded("connect_timeout");
    // leaves (Re)connecting -> disarms watchdog
    SetState(StateForConfig(config_), "connect_timeout");
    ScheduleChannelReconnect("connect_timeout");
}

void EidolonVoiceController::UpdateIdleAutoLeave()
{
    if (!ptt_mode_ || kPttIdleFallbackUs == 0) {
        if (idle_leave_timer_ != nullptr) {
            esp_timer_stop(idle_leave_timer_);
        }
        return;
    }
    // Idle = in the voice room, PTT mode, nobody holding the button, and the agent
    // is not producing output. Any of those changing re-evaluates the timer.
    bool agent_output_active = agent_phase_ != AgentPhase::Silent ||
                               local_playback_ui_active_ || PlaybackActiveRecently();
    bool idle = state_ == VoiceSessionState::InRoom && !standby_ && !ptt_active_ &&
                !agent_output_active;
    if (idle) {
        if (idle_leave_timer_ == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = &EidolonVoiceController::IdleLeaveCb;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "eidolon_idle_lv";
            if (esp_timer_create(&args, &idle_leave_timer_) != ESP_OK) {
                idle_leave_timer_ = nullptr;
                return;
            }
        }
        esp_timer_stop(idle_leave_timer_);
        esp_timer_start_once(idle_leave_timer_, kPttIdleFallbackUs);
    } else if (idle_leave_timer_ != nullptr) {
        esp_timer_stop(idle_leave_timer_);
    }
}

void EidolonVoiceController::IdleLeaveCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::IdleLeave;
    self->Enqueue(ev);
}

void EidolonVoiceController::PttReleaseTailCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::PttReleaseTail;
    self->Enqueue(ev);
}

void EidolonVoiceController::AmbientPresenceTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::AmbientPresenceTimer;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoIdleAutoLeave()
{
    // Activity may have resumed between the timer firing and now.
    bool agent_output_active = agent_phase_ != AgentPhase::Silent ||
                               local_playback_ui_active_ || PlaybackActiveRecently();
    if (!(ptt_mode_ && kPttIdleFallbackUs > 0 && state_ == VoiceSessionState::InRoom &&
          !standby_ && !ptt_active_ && !agent_output_active)) {
        return;
    }
    ESP_LOGI(TAG,
             "[lifecycle] ptt idle fallback: leaving voice room after %llus idle "
             "room=%s gen=%lu",
             kPttIdleFallbackUs / 1000000ULL, config_.session.room_name.c_str(),
             static_cast<unsigned long>(session_generation_));
    HandleSessionEnd(EndReason::IdleNormalEnd);  // -> standby; re-connect is an explicit tap
}

void EidolonVoiceController::ResetFullDuplexIdleFallback(const char* reason)
{
    // Presence-managed sessions are externally bounded by the renewable owner
    // lease. The 75s client safety fallback is only for normal voice sessions;
    // applying it here would still eject a silent/stationary owner.
    if (ptt_mode_ || kFullDuplexIdleFallbackUs == 0 ||
        presence_managed_voice_session_) {
        DisarmFullDuplexIdleFallback();
        return;
    }
    if (state_ != VoiceSessionState::InRoom || standby_ || !session_.IsConnected()) {
        DisarmFullDuplexIdleFallback();
        return;
    }
    if (full_duplex_idle_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::FullDuplexIdleFallbackCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_fd_idle";
        if (esp_timer_create(&args, &full_duplex_idle_timer_) != ESP_OK) {
            full_duplex_idle_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create full-duplex idle fallback timer");
            return;
        }
    }
    esp_timer_stop(full_duplex_idle_timer_);
    if (esp_timer_start_once(full_duplex_idle_timer_, kFullDuplexIdleFallbackUs) == ESP_OK) {
        ESP_LOGD(TAG, "[lifecycle] full-duplex idle fallback armed reason=%s timeout=%lums",
                 reason ? reason : "activity",
                 static_cast<unsigned long>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS));
    }
}

void EidolonVoiceController::DisarmFullDuplexIdleFallback()
{
    if (full_duplex_idle_timer_ != nullptr) {
        esp_timer_stop(full_duplex_idle_timer_);
    }
}

void EidolonVoiceController::FullDuplexIdleFallbackCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::FullDuplexIdleFallback;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoFullDuplexIdleFallback()
{
    if (ptt_mode_ || state_ != VoiceSessionState::InRoom || standby_ ||
        !session_.IsConnected() || presence_managed_voice_session_) {
        return;
    }
    if (agent_phase_ != AgentPhase::Silent || AgentOutputActiveRecently()) {
        ResetFullDuplexIdleFallback("activity_still_active");
        return;
    }
    ESP_LOGW(TAG,
             "[lifecycle] full-duplex idle fallback fired after %lums; "
             "no session_end/room-close observed, returning to standby=%s",
             static_cast<unsigned long>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS),
             config_.session.room_name.c_str());
    HandleSessionEnd(EndReason::IdleNormalEnd);
}

// ============================ Control commands ============================

esp_err_t EidolonVoiceController::AckCommand(const ControlCommand& command, const char* status,
                                             const char* code, const char* detail, const char* result)
{
    const esp_err_t err = session_.PublishData(
        kControlTopic,
        BuildControlAck(command, OperationalDeviceInstanceId(), status, code, detail, result));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control ACK publish failed op=%s id=%s status=%s code=%s err=%s",
                 command.op.c_str(), command.id.c_str(), status, code, esp_err_to_name(err));
    }
    return err;
}

void EidolonVoiceController::CompletePendingRoomJoinCommand(const char* status,
                                                            const char* code,
                                                            const char* detail,
                                                            const char* result)
{
    if (!pending_room_join_command_active_) {
        return;
    }
    AckCommand(pending_room_join_command_, status, code, detail, result);
    pending_room_join_command_active_ = false;
    pending_room_join_generation_ = 0;
    pending_room_join_command_ = ControlCommand{};
}

const char* EidolonVoiceController::JoinBlockedCode() const
{
    switch (config_.status) {
    case HubConfigStatus::PendingApproval:
        return "NEEDS_APPROVAL";
    case HubConfigStatus::WaitingBinding:
        return "NEEDS_BINDING";
    case HubConfigStatus::RecoveryRequired:
    case HubConfigStatus::Revoked:
        return "UNAUTHORIZED";
    case HubConfigStatus::Active:
        break;
    }
    return "ROOM_JOIN_FAILED";
}

void EidolonVoiceController::DoControlCommand(const std::string& payload)
{
    ControlCommand command = ParseControlCommand(payload);
    if (!command.valid) {
        ESP_LOGW(TAG, "Ignoring malformed control command");
        return;
    }
    if (command.expired) {
        ESP_LOGW(TAG, "Ignoring expired control command op=%s", command.op.c_str());
        AckCommand(command, "expired", "COMMAND_EXPIRED");
        return;
    }
    // Op dispatch registry. Each handler runs inline on the controller task (no
    // per-command worker task); blocking work just queues other events briefly.
    struct ControlOpHandler {
        const char* op;
        int capability_version;
        void (EidolonVoiceController::*handler)(const std::string&, const std::string&);
    };
    static const ControlOpHandler kControlOps[] = {
        {kControlOpConfigRefresh, 0, &EidolonVoiceController::HandleConfigRefreshCommand},
        {kControlOpRoomJoin, 0, &EidolonVoiceController::HandleRoomJoinCommand},
        {kControlOpPlaybackStop, 0, &EidolonVoiceController::HandlePlaybackStopCommand},
        {kControlOpPttTurnStatus, 0, &EidolonVoiceController::HandlePttTurnStatusCommand},
        {kControlOpDeviceIdentify, 1, &EidolonVoiceController::HandleDeviceIdentifyCommand},
        {kControlOpHeadLookAt, 1, &EidolonVoiceController::HandleHeadLookAtCommand},
        {kControlOpHeadHome, 1, &EidolonVoiceController::HandleHeadHomeCommand},
        {kControlOpHeadGesture, 1, &EidolonVoiceController::HandleHeadGestureCommand},
        {kControlOpSafetyStop, 1, &EidolonVoiceController::HandleSafetyStopCommand},
        {kControlOpPresenceSet, 1, &EidolonVoiceController::HandlePresenceSetCommand},
#if CONFIG_EIDOLON_GUARD_SERVICE
        {kControlOpDeviceRollCall, 1, &EidolonVoiceController::HandleDeviceRollCallCommand},
        {kControlOpGuardRuntimeSync, 0, &EidolonVoiceController::HandleGuardRuntimeSyncCommand},
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        {kControlOpGuardOwnerFaceProfileSync, 0,
         &EidolonVoiceController::HandleGuardOwnerFaceProfileSyncCommand},
#endif
#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
        {kControlOpGuardVisionBenchmark, 0,
         &EidolonVoiceController::HandleGuardVisionBenchmarkCommand},
#endif
    };

    for (const auto& entry : kControlOps) {
        if (command.op == entry.op) {
            if (entry.capability_version > 0 && command.capability_version > 0 &&
                command.capability_version != entry.capability_version) {
                ESP_LOGW(TAG,
                         "Unsupported capability version op=%s expected=%d got=%d",
                         command.op.c_str(), entry.capability_version,
                         command.capability_version);
                AckCommand(command, "unsupported", "UNSUPPORTED_CAPABILITY_VERSION");
                return;
            }
            AckCommand(command, "accepted", "OK");
            (this->*entry.handler)(command.id, command.payload);
            return;
        }
    }

    ESP_LOGI(TAG, "Unsupported control command op=%s", command.op.c_str());
    AckCommand(command, "unsupported", "UNSUPPORTED_OP");
}

EndReason EidolonVoiceController::ParseEndReason(const std::string& payload)
{
    // Channel sends {"type":"session_end","reason":"<reason>"} on
    // eidolon.session_control before tearing the room down. Legacy channels sent
    // {"type":"idle_timeout","reason":"idle_timeout"} — treat that as a normal end.
    EndReason reason = EndReason::None;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        return reason;
    }
    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    cJSON* reason_item = cJSON_GetObjectItem(root, "reason");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const char* r = cJSON_IsString(reason_item) ? reason_item->valuestring : "";

    if (strcmp(type, kSessionEndType) == 0) {
        if (strcmp(r, kSessionEndIdleNormal) == 0) {
            reason = EndReason::IdleNormalEnd;
        } else if (strcmp(r, kSessionEndProactiveDone) == 0) {
            reason = EndReason::ProactiveDone;
        } else if (strcmp(r, kSessionEndUserLeft) == 0) {
            reason = EndReason::UserLeft;
        } else if (strcmp(r, kSessionEndSuperseded) == 0) {
            reason = EndReason::Superseded;
        } else if (strcmp(r, kSessionEndError) == 0) {
            reason = EndReason::Error;
        } else {
            // Unknown reason from a newer channel: treat as a normal end (return
            // to Ready) rather than guessing an error.
            reason = EndReason::IdleNormalEnd;
        }
    } else if (strcmp(type, "idle_timeout") == 0 || strcmp(r, "idle_timeout") == 0) {
        reason = EndReason::IdleNormalEnd;  // legacy
    }
    cJSON_Delete(root);
    return reason;
}

std::string EidolonVoiceController::NewConversationId()
{
    char id[48];
    conversation_sequence_ += 1;
    snprintf(id, sizeof(id), "esp32-%08lx-%08lx-%08lx",
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(conversation_sequence_));
    return id;
}

esp_err_t EidolonVoiceController::PublishSessionRequest(
    const char* type, const std::string& conversation_id)
{
    // Asking is a statement of desired state, not an event: the device may say
    // the same thing twice — after a retry, or after a reconnection it is not
    // sure the server noticed — and mean it once.
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_v", kWireSchemaVersion);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, kSessionConversationIdField, conversation_id.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = session_.PublishData(kSessionControlTopic, printed, /*reliable=*/true);
    cJSON_free(printed);
    ESP_LOGI(TAG, "[lifecycle] sent %s conversation_id=%s err=%s gen=%lu", type,
             conversation_id.c_str(), esp_err_to_name(err),
             static_cast<unsigned long>(session_generation_));
    return err;
}

void EidolonVoiceController::DoSessionControl(const std::string& payload)
{
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Ignoring malformed session_control payload");
        return;
    }
    const cJSON* schema = cJSON_GetObjectItem(root, "schema_v");
    const cJSON* type_item = cJSON_GetObjectItem(root, "type");
    const cJSON* conversation_item =
        cJSON_GetObjectItem(root, kSessionConversationIdField);
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : nullptr;
    const char* conversation_id = cJSON_IsString(conversation_item)
                                      ? conversation_item->valuestring
                                      : nullptr;
    const bool valid_envelope = cJSON_IsNumber(schema) &&
                                schema->valueint == kWireSchemaVersion && type != nullptr &&
                                conversation_id != nullptr && conversation_id[0] != '\0';
    if (!valid_envelope) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Ignoring invalid session_control envelope");
        return;
    }
    if (current_conversation_id_.empty() ||
        current_conversation_id_ != conversation_id) {
        ESP_LOGI(TAG,
                 "Ignoring stale session_control type=%s conversation_id=%s current=%s",
                 type, conversation_id,
                 current_conversation_id_.empty() ? "<none>"
                                                  : current_conversation_id_.c_str());
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, kSessionStartedType) == 0) {
        cJSON_Delete(root);
        if (state_ != VoiceSessionState::Opening &&
            state_ != VoiceSessionState::Connecting &&
            state_ != VoiceSessionState::Reconnecting) {
            ESP_LOGI(TAG, "Ignoring duplicate session_started in state=%s",
                     VoiceStateName(state_));
            return;
        }
        standby_ = false;
        conversation_confirmed_ = true;
        channel_recovery_.OnConversationStarted();
        SetOperationalReady(true, "session_started");
        SetState(VoiceSessionState::InRoom, "session_started");
        SetPresenceWakePhase(PresenceWakePhase::Idle);
        OpenConversationAudio();
        if (pending_room_join_command_active_) {
            char result[224];
            snprintf(result, sizeof(result),
                     "{\"status\":\"in_room\",\"conversation_id\":\"%s\","
                     "\"generation\":%lu}",
                     current_conversation_id_.c_str(),
                     static_cast<unsigned long>(session_generation_));
            CompletePendingRoomJoinCommand("completed", "OK", "", result);
        }
        return;
    }
    cJSON_Delete(root);

    EndReason reason = ParseEndReason(payload);
    if (reason == EndReason::None) {
        ESP_LOGI(TAG, "Ignoring unsupported session_control payload");
        return;
    }
    HandleSessionEnd(reason);
}

void EidolonVoiceController::DoDeviceEvent(const std::string& payload,
                                           uint32_t event_generation)
{
    if (event_generation != session_generation_) {
        ESP_LOGD(TAG,
                 "Dropped stale device event generation=%lu current=%lu",
                 static_cast<unsigned long>(event_generation),
                 static_cast<unsigned long>(session_generation_));
        return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t now_epoch_ms =
        now >= 1'700'000'000'000LL ? static_cast<uint64_t>(now) : 0;
    const auto result = device_event_bus_.Dispatch(payload, now_epoch_ms);
    if (result == DeviceEventDispatchResult::Invalid) {
        ESP_LOGW(TAG, "Rejected malformed device event");
    } else if (result == DeviceEventDispatchResult::Expired) {
        ESP_LOGD(TAG, "Ignored expired device event");
    }
}

void EidolonVoiceController::DoPublishDeviceEvent(const std::string& payload)
{
    if (!standby_ || !session_.IsConnected()) {
        ESP_LOGD(TAG, "Dropped volatile device event: channel unavailable");
        return;
    }
    const esp_err_t err = session_.PublishData(kEventTopic, payload, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device event publish failed: %s", esp_err_to_name(err));
    }
}

void EidolonVoiceController::DoAmbientPresenceChanged(bool present)
{
#if !CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    (void)present;
    return;
#else
    const uint64_t monotonic_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (radar_presence_known_ && radar_present_ == present) {
        return;
    }
    radar_presence_known_ = true;
    radar_present_ = present;
    radar_presence_dirty_ = true;
    radar_pending_observation_ = AmbientPresenceObservation::Edge;
    radar_activation_gate_.ResetForRadarTransition();
    if (present) {
        ++radar_presence_epoch_;
        if (radar_presence_epoch_ == 0) {
            radar_presence_epoch_ = 1;
        }
        radar_presence_flow_id_ =
            MakeDeviceEventId("flow-presence", monotonic_ms, esp_random());
        SetPresenceWakePhase(PresenceWakePhase::VerifyingOwner);
    } else {
        if (radar_presence_epoch_ == 0) {
            radar_presence_epoch_ = 1;
        }
        SetPresenceWakePhase(PresenceWakePhase::Idle);
    }
    PublishRadarPresenceState(AmbientPresenceObservation::Edge);
#endif
}

void EidolonVoiceController::PublishRadarPresenceState(
    AmbientPresenceObservation observation)
{
#if !CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    (void)observation;
    return;
#else
    if (!radar_presence_known_) {
        return;
    }
    if (!standby_ || !session_.IsConnected()) {
        radar_presence_dirty_ = true;
        if (radar_pending_observation_ !=
            AmbientPresenceObservation::Edge) {
            radar_pending_observation_ =
                AmbientPresenceObservation::Snapshot;
        }
        return;
    }
    const uint64_t monotonic_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const auto epoch_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (epoch_now < 1'700'000'000'000LL) {
        radar_presence_dirty_ = true;
        radar_pending_observation_ =
            AmbientPresenceObservation::Snapshot;
        ScheduleAmbientPresenceTimer(kRadarStatePublishRetryMs);
        return;
    }

    if (radar_presence_flow_id_.empty()) {
        radar_presence_flow_id_ =
            MakeDeviceEventId("flow-presence", monotonic_ms, esp_random());
    }
    ++radar_presence_sequence_;
    if (radar_presence_sequence_ == 0) {
        radar_presence_sequence_ = 1;
    }
    const std::string event_id =
        MakeDeviceEventId("evt-radar", monotonic_ms, esp_random());
    char state_payload[256] = {};
    const int payload_length = std::snprintf(
        state_payload, sizeof(state_payload),
        "{\"state\":\"%s\",\"modality\":\"mmwave\","
        "\"presence_epoch\":%lu,\"sequence\":%lu,\"lease_ms\":%lu,"
        "\"observation\":\"%s\"}",
        radar_present_ ? "present" : "vacant",
        static_cast<unsigned long>(radar_presence_epoch_),
        static_cast<unsigned long>(radar_presence_sequence_),
        static_cast<unsigned long>(
            radar_present_ ? kRadarPresenceLeaseMs : 0),
        AmbientPresenceObservationName(observation));
    if (payload_length <= 0 ||
        static_cast<size_t>(payload_length) >= sizeof(state_payload)) {
        radar_presence_dirty_ = true;
        radar_pending_observation_ =
            AmbientPresenceObservation::Snapshot;
        ScheduleAmbientPresenceTimer(kRadarStatePublishRetryMs);
        return;
    }
    const std::string event_json = BuildDeviceEventJson({
        .event_id = event_id,
        .flow_id = radar_presence_flow_id_,
        .causation_id = "",
        .type = kAmbientPresenceStateType,
        .source_device_id = OperationalDeviceInstanceId(),
        .source_component = "radar",
        .occurred_at_ms = static_cast<uint64_t>(epoch_now),
        .expires_at_ms =
            static_cast<uint64_t>(epoch_now) + DeviceEventBus::kMaxTtlMs,
        .payload_json = state_payload,
    });
    if (event_json.empty() ||
        session_.PublishData(kEventTopic, event_json, true) != ESP_OK) {
        radar_presence_dirty_ = true;
        radar_pending_observation_ =
            AmbientPresenceObservation::Snapshot;
        ESP_LOGW(TAG, "Radar presence state publish failed");
        ScheduleAmbientPresenceTimer(kRadarStatePublishRetryMs);
        return;
    }
    radar_presence_dirty_ = false;
    radar_pending_observation_ =
        AmbientPresenceObservation::Snapshot;
    ESP_LOGI(TAG,
             "radar_presence_state state=%s observation=%s epoch=%lu sequence=%lu flow_id=%s",
             radar_present_ ? "present" : "vacant",
             AmbientPresenceObservationName(observation),
             static_cast<unsigned long>(radar_presence_epoch_),
             static_cast<unsigned long>(radar_presence_sequence_),
             radar_presence_flow_id_.c_str());
    if (radar_present_) {
        ScheduleAmbientPresenceTimer(kRadarPresenceHeartbeatMs);
    } else if (ambient_presence_timer_ != nullptr) {
        esp_timer_stop(ambient_presence_timer_);
    }
#endif
}

void EidolonVoiceController::ScheduleAmbientPresenceTimer(uint64_t delay_ms)
{
    if (ambient_presence_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(ambient_presence_timer_);
    esp_timer_start_once(ambient_presence_timer_,
                         std::max<uint64_t>(delay_ms, 1) * 1000ULL);
}

void EidolonVoiceController::HandleOwnerPresenceConfirmedEvent(
    const DeviceEventMessage& event)
{
#if !CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    (void)event;
    return;
#else
    std::string ambient_source_device_id;
    uint32_t ambient_presence_epoch = 0;
    uint32_t lease_ms = 0;
    uint32_t guard_epoch = 0;
    uint32_t presence_sequence = 0;
    if (!standby_ || !session_.IsConnected() ||
        !radar_presence_known_ || !radar_present_ ||
        !ParseOwnerConfirmationLease(
            event.payload_json, &ambient_source_device_id,
            &ambient_presence_epoch, &lease_ms, &guard_epoch,
            &presence_sequence) ||
        event.flow_id != radar_presence_flow_id_ ||
        event.causation_id.empty() ||
        ambient_source_device_id != OperationalDeviceInstanceId() ||
        ambient_presence_epoch != radar_presence_epoch_) {
        return;
    }
    if (!radar_activation_gate_.TryConsume(
            radar_presence_epoch_, event.source_device_id,
            event.occurred_at_ms, guard_epoch, presence_sequence)) {
        return;
    }
    const uint64_t now_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (presence_managed_voice_session_) {
        if (event.source_device_id == owner_lease_source_device_id_ &&
            (guard_epoch > owner_lease_guard_epoch_ ||
             (guard_epoch == owner_lease_guard_epoch_ &&
              presence_sequence > owner_lease_sequence_))) {
            owner_lease_guard_epoch_ = guard_epoch;
            owner_lease_sequence_ = presence_sequence;
            owner_lease_deadline_ms_ = now_ms + lease_ms;
        }
        return;
    }
    ESP_LOGI(TAG,
             "owner_confirmation_received flow_id=%s t8_ms=%llu",
             event.flow_id.c_str(),
             static_cast<unsigned long long>(now_ms));
    SetPresenceWakePhase(PresenceWakePhase::OwnerRecognized);
#if CONFIG_EIDOLON_OWNER_PRESENCE_VOICE_WAKE
    presence_managed_voice_session_ = true;
    current_presence_flow_id_ = event.flow_id;
    owner_lease_source_device_id_ = event.source_device_id;
    owner_lease_deadline_ms_ = now_ms + lease_ms;
    owner_lease_guard_epoch_ = guard_epoch;
    owner_lease_sequence_ = presence_sequence;
    pending_session_intent_ = kSessionIntentPresence;
    pending_session_flow_id_ = event.flow_id;
    PublishFlowNode(event.flow_id, event.event_id, "box.voice_join",
                    "running", "BOX joining Voice Room");
    const esp_err_t join_err = DoJoinRoom();
    pending_session_intent_.clear();
    pending_session_flow_id_.clear();
    if (join_err != ESP_OK) {
        ESP_LOGW(TAG, "Owner-confirmed automatic join failed: %s",
                 esp_err_to_name(join_err));
        // Authentication remains valid and owner lifecycle heartbeats retain
        // authority to recover a transient Voice Room join failure. Do not
        // reopen visual authentication or synthesize a radar retry.
        SetPresenceWakePhase(PresenceWakePhase::OwnerRecognized);
    }
#endif
#endif
}

void EidolonVoiceController::HandleOwnerPresenceChangedEvent(
    const DeviceEventMessage& event)
{
#if !CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    (void)event;
#else
    bool present = false;
    uint32_t lease_ms = 0;
    uint32_t guard_epoch = 0;
    uint32_t presence_sequence = 0;
    if (!ParseOwnerPresenceChanged(event.payload_json, &present, &lease_ms,
                                   &guard_epoch, &presence_sequence)) {
        return;
    }
    const auto lifecycle_result =
        radar_activation_gate_.ApplyOwnerLifecycle(
            event.source_device_id, present, event.occurred_at_ms,
            guard_epoch, presence_sequence);
    if (lifecycle_result ==
        AmbientOwnerLifecycleApplyResult::Rejected) {
        return;
    }
    if (lifecycle_result == AmbientOwnerLifecycleApplyResult::Stale) {
        ESP_LOGD(TAG,
                 "[companion] ignored stale owner lease epoch=%lu sequence=%lu",
                 static_cast<unsigned long>(guard_epoch),
                 static_cast<unsigned long>(presence_sequence));
        return;
    }
    if (!presence_managed_voice_session_) {
        if (!present) {
            SetPresenceWakePhase(
                radar_presence_known_ && radar_present_
                    ? PresenceWakePhase::VerifyingOwner
                    : PresenceWakePhase::Idle);
        }
        return;
    }
    owner_lease_guard_epoch_ = guard_epoch;
    owner_lease_sequence_ = presence_sequence;
    const uint64_t now_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (present) {
        owner_lease_deadline_ms_ = now_ms + lease_ms;
        ESP_LOGI(TAG,
                 "[companion] owner lease renewed source=%s lease_ms=%lu",
                 event.source_device_id.c_str(),
                 static_cast<unsigned long>(lease_ms));
        // A transient Voice Room/network failure falls back to the always-on
        // Control Room without revoking the authenticated companion session.
        // A fresh, source-scoped owner heartbeat is the recovery authority:
        // rejoin only while the original managed session is still active.
        // Explicit EXIT/session_end/owner-absent all reset that state first and
        // therefore can never take this branch.
        if (standby_ && session_.IsConnected() &&
            state_ == VoiceSessionState::ConfigReady &&
            !current_presence_flow_id_.empty()) {
            ESP_LOGI(TAG,
                     "[companion] fresh owner lease recovering dropped Voice Room flow_id=%s",
                     current_presence_flow_id_.c_str());
            pending_session_intent_ = kSessionIntentPresence;
            pending_session_flow_id_ = current_presence_flow_id_;
            PublishFlowNode(current_presence_flow_id_, event.event_id,
                            "box.voice_rejoin", "running",
                            "BOX recovering Voice Room");
            const esp_err_t join_err = DoJoinRoom();
            pending_session_intent_.clear();
            pending_session_flow_id_.clear();
            if (join_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "Owner-lease Voice Room recovery failed: %s",
                         esp_err_to_name(join_err));
            }
        }
        return;
    }
    ESP_LOGI(TAG,
             "[companion] owner absent source=%s; closing presence session",
             event.source_device_id.c_str());
    ResetPresenceManagedSession();
    SetPresenceWakePhase(
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
        radar_presence_known_ && radar_present_
            ? PresenceWakePhase::VerifyingOwner
            :
#endif
              PresenceWakePhase::Idle);
    if (!standby_ && state_ == VoiceSessionState::InRoom) {
        HandleSessionEnd(EndReason::IdleNormalEnd);
    }
#endif
}

void EidolonVoiceController::PublishFlowNode(
    const std::string& flow_id, const std::string& causation_id,
    const char* stage, const char* status, const char* label)
{
    if (flow_id.empty() || stage == nullptr || status == nullptr ||
        label == nullptr) {
        return;
    }
    const auto epoch_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (epoch_now < 1'700'000'000'000LL) {
        return;
    }
    char payload[256] = {};
    const int written = std::snprintf(
        payload, sizeof(payload),
        "{\"stage\":\"%s\",\"status\":\"%s\",\"label\":\"%s\"}",
        stage, status, label);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
        return;
    }
    const uint64_t monotonic_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const std::string event_json = BuildDeviceEventJson({
        .event_id = MakeDeviceEventId("evt-flow", monotonic_ms, esp_random()),
        .flow_id = flow_id,
        .causation_id = causation_id,
        .type = kCompanionFlowNodeType,
        .source_device_id = OperationalDeviceInstanceId(),
        .source_component = "companion_lifecycle",
        .occurred_at_ms = static_cast<uint64_t>(epoch_now),
        .expires_at_ms = static_cast<uint64_t>(epoch_now) +
                         DeviceEventBus::kMaxTtlMs,
        .payload_json = payload,
    });
    if (!event_json.empty()) {
        DoPublishDeviceEvent(event_json);
    }
}

void EidolonVoiceController::ResetPresenceManagedSession()
{
    presence_managed_voice_session_ = false;
    current_presence_flow_id_.clear();
    owner_lease_source_device_id_.clear();
    owner_lease_deadline_ms_ = 0;
    owner_lease_guard_epoch_ = 0;
    owner_lease_sequence_ = 0;
}

void EidolonVoiceController::CheckOwnerPresenceLease()
{
    if (!presence_managed_voice_session_ || standby_ ||
        state_ != VoiceSessionState::InRoom ||
        owner_lease_deadline_ms_ == 0) {
        return;
    }
    const uint64_t now_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (now_ms <= owner_lease_deadline_ms_) {
        return;
    }
    ESP_LOGW(TAG,
             "[companion] owner lease expired source=%s; fail-safe closing voice",
             owner_lease_source_device_id_.c_str());
    ResetPresenceManagedSession();
    HandleSessionEnd(EndReason::IdleNormalEnd);
}

void EidolonVoiceController::DoAmbientPresenceTimer()
{
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    const uint64_t now_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
#endif
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    if (radar_presence_known_ && standby_ && session_.IsConnected()) {
        PublishRadarPresenceState(
            radar_presence_dirty_
                ? AmbientPresenceObservation::Snapshot
                : AmbientPresenceObservation::Heartbeat);
    }
#endif
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    const auto expired = ambient_presence_registry_.Expire(now_ms);
    for (const auto& assertion : expired) {
        ESP_LOGI(TAG,
                 "ambient_presence_lease_expired source=%s epoch=%lu flow_id=%s",
                 assertion.source_device_id.c_str(),
                 static_cast<unsigned long>(assertion.presence_epoch),
                 assertion.flow_id.c_str());
        PublishFlowNode(assertion.flow_id, assertion.root_event_id,
                        "atk.presence_lease", "timeout",
                        "ATK ambient presence lease expired");
    }
    ScheduleAmbientPresenceLeaseExpiry(now_ms);
#endif
}

void EidolonVoiceController::SetPresenceWakePhase(PresenceWakePhase phase)
{
    if (presence_wake_phase_ == phase) {
        return;
    }
    presence_wake_phase_ = phase;
    if (on_presence_wake_phase_) {
        on_presence_wake_phase_(phase);
    }
}

#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
void EidolonVoiceController::ScheduleAmbientPresenceLeaseExpiry(
    uint64_t now_ms)
{
    const uint64_t deadline =
        ambient_presence_registry_.NextLeaseDeadlineMs();
    if (deadline == 0) {
        if (ambient_presence_timer_ != nullptr) {
            esp_timer_stop(ambient_presence_timer_);
        }
        return;
    }
    ScheduleAmbientPresenceTimer(
        deadline > now_ms ? deadline - now_ms : 1);
}
#endif

#if CONFIG_EIDOLON_GUARD_SERVICE
void EidolonVoiceController::HandleAmbientPresenceEvent(
    const DeviceEventMessage& event)
{
#if !CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    (void)event;
    return;
#else
    if (guard_service_ == nullptr || !standby_ ||
        !session_.IsConnected()) {
        return;
    }
    AmbientPresenceState state;
    if (!ParseAmbientPresenceState(event.payload_json, &state)) {
        ESP_LOGW(TAG, "Rejected malformed ambient presence state");
        return;
    }
    const uint64_t now_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const auto result = ambient_presence_registry_.Apply(
        event.source_device_id, event.flow_id, event.event_id, state,
        event.occurred_at_ms, now_ms);
    if (result == AmbientPresenceApplyResult::Rejected) {
        ESP_LOGW(TAG, "Rejected invalid ambient presence assertion");
        return;
    }
    if (result == AmbientPresenceApplyResult::Stale) {
        ESP_LOGD(TAG,
                 "Ignored stale ambient presence source=%s epoch=%lu sequence=%lu",
                 event.source_device_id.c_str(),
                 static_cast<unsigned long>(state.presence_epoch),
                 static_cast<unsigned long>(state.sequence));
        return;
    }

    if (result == AmbientPresenceApplyResult::Armed) {
        ESP_LOGI(TAG,
                 "ambient_presence_armed flow_id=%s source=%s epoch=%lu",
                 event.flow_id.c_str(), event.source_device_id.c_str(),
                 static_cast<unsigned long>(state.presence_epoch));
        PublishFlowNode(event.flow_id, event.event_id,
                        "atk.owner_watch", "running",
                        "ATK armed, awaiting owner");
    } else if (result == AmbientPresenceApplyResult::Disarmed) {
        ESP_LOGI(TAG,
                 "ambient_presence_disarmed flow_id=%s source=%s epoch=%lu",
                 event.flow_id.c_str(), event.source_device_id.c_str(),
                 static_cast<unsigned long>(state.presence_epoch));
        PublishFlowNode(event.flow_id, event.event_id,
                        "atk.presence_disarmed", "completed",
                        "ATK ambient presence ended");
    }
    ScheduleAmbientPresenceLeaseExpiry(now_ms);

    if (state.present) {
        const OwnerPresenceObservation owner_presence =
            guard_service_->CurrentOwnerPresence();
        if (owner_presence.state == OwnerPresenceState::Present) {
            PublishPendingOwnerConfirmations(owner_presence);
        }
    }
#endif
}

#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
void EidolonVoiceController::PublishPendingOwnerConfirmations(
    const OwnerPresenceObservation& owner_presence)
{
    if (owner_presence.state != OwnerPresenceState::Present ||
        owner_presence.profile_revision == 0 ||
        owner_presence.sequence == 0 || guard_service_ == nullptr) {
        return;
    }
    const auto pending =
        ambient_presence_registry_.PendingOwnerConfirmations();
    for (const auto& assertion : pending) {
        if (PublishOwnerConfirmation(assertion, owner_presence)) {
            ambient_presence_registry_.MarkOwnerConfirmed(
                assertion.source_device_id, assertion.presence_epoch);
        }
    }
}

bool EidolonVoiceController::PublishOwnerConfirmation(
    const AmbientPresenceAssertion& assertion,
    const OwnerPresenceObservation& owner_presence)
{
    if (!standby_ || !session_.IsConnected() || guard_service_ == nullptr) {
        return false;
    }
    const auto epoch_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (epoch_now < 1'700'000'000'000LL) {
        return false;
    }
    char payload[448] = {};
    const int payload_length = std::snprintf(
        payload, sizeof(payload),
        "{\"ambient_source_device_id\":\"%s\","
        "\"ambient_presence_epoch\":%lu,"
        "\"profile_revision\":%lu,\"guard_epoch\":%lu,"
        "\"presence_sequence\":%lu,\"evidence\":\"local_owner_face\","
        "\"raw_retention\":\"none\",\"lease_ms\":%lu}",
        assertion.source_device_id.c_str(),
        static_cast<unsigned long>(assertion.presence_epoch),
        static_cast<unsigned long>(owner_presence.profile_revision),
        static_cast<unsigned long>(owner_presence.epoch),
        static_cast<unsigned long>(owner_presence.sequence),
        static_cast<unsigned long>(guard_service_->OwnerPresenceLeaseMs()));
    if (payload_length <= 0 ||
        static_cast<size_t>(payload_length) >= sizeof(payload)) {
        return false;
    }
    const uint64_t monotonic_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const std::string event_json = BuildDeviceEventJson({
        .event_id = MakeDeviceEventId("evt-owner", monotonic_ms, esp_random()),
        .flow_id = assertion.flow_id,
        .causation_id = assertion.root_event_id,
        .type = kIdentityOwnerPresenceConfirmedType,
        .source_device_id = OperationalDeviceInstanceId(),
        .source_component = "owner_face",
        .occurred_at_ms = static_cast<uint64_t>(epoch_now),
        .expires_at_ms =
            static_cast<uint64_t>(epoch_now) + DeviceEventBus::kMaxTtlMs,
        .payload_json = payload,
    });
    if (event_json.empty()) {
        return false;
    }
    const esp_err_t err =
        session_.PublishData(kEventTopic, event_json, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Owner confirmation publish failed flow_id=%s: %s",
                 assertion.flow_id.c_str(), esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG,
             "owner_confirmation_published flow_id=%s radar_source=%s owner_epoch=%lu",
             assertion.flow_id.c_str(), assertion.source_device_id.c_str(),
             static_cast<unsigned long>(owner_presence.epoch));
    return true;
}
#endif
#endif

void EidolonVoiceController::HandleConfigRefreshCommand(const std::string& command_id,
                                                        const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> refresh Hub config");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpConfigRefresh;

    if (RefreshHubConfig() != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered config refresh failed");
        AckCommand(command, "failed", "CONFIG_REFRESH_FAILED");
        return;
    }

    if (config_.status == HubConfigStatus::Active) {
        AckCommand(command, "succeeded", "OK", "", "{\"status\":\"active\"}");
        vTaskDelay(kActiveAckSettleDelay);
        ConnectChannel();
        return;
    }

    AckCommand(command, "succeeded", "OK");
    ConnectChannel();
}

#if CONFIG_EIDOLON_GUARD_SERVICE
void EidolonVoiceController::HandleGuardRuntimeSyncCommand(const std::string& command_id,
                                                           const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardRuntimeSync;
    if (guard_service_ == nullptr) {
        AckCommand(command, "failed", "GUARD_RUNTIME_UNAVAILABLE");
        return;
    }
    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "runtime_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_runtime_state") : nullptr;
    bool only_known_fields = root != nullptr;
    for (const cJSON* item = root ? root->child : nullptr; item != nullptr; item = item->next) {
        if (strcmp(item->string ? item->string : "", "binding_id") != 0 &&
            strcmp(item->string ? item->string : "", "runtime_revision") != 0 &&
            strcmp(item->string ? item->string : "", "desired_runtime_state") != 0) {
            only_known_fields = false;
            break;
        }
    }
    const bool valid = only_known_fields && cJSON_IsString(binding_id) && binding_id->valuestring &&
                       cJSON_IsNumber(revision) && revision->valueint > 0 &&
                       revision->valuedouble == static_cast<double>(revision->valueint) &&
                       cJSON_IsString(desired) && desired->valuestring &&
                       (strcmp(desired->valuestring, "running") == 0 ||
                        strcmp(desired->valuestring, "stopped") == 0);
    if (!valid) {
        cJSON_Delete(root);
        AckCommand(command, "failed", "INVALID_ARGUMENT");
        return;
    }
    const std::string expected_binding_id = binding_id->valuestring;
    const std::string expected_desired_state = desired->valuestring;
    const uint32_t expected_revision = static_cast<uint32_t>(revision->valueint);
    cJSON_Delete(root);

    if (expected_desired_state == "stopped") {
        ClearGuardPresenceRuntime();
        guard_service_->Stop("hub_runtime_stopped");
        has_guard_control_config_ = false;
        guard_control_config_ = RoomConfig{};
        AckCommand(command, "completed", "OK", "",
                   ("{\"binding_id\":\"" + expected_binding_id +
                    "\",\"runtime_revision\":" + std::to_string(expected_revision) +
                    ",\"desired_runtime_state\":\"stopped\",\"running\":false}").c_str());
        vTaskDelay(kActiveAckSettleDelay);
        if (RefreshHubConfig(/*persist=*/false) == ESP_OK) {
            ConnectChannel();
        }
        return;
    }

    uint32_t applied_revision = 0;
    if (SyncGuardRuntime("hub_runtime_sync", &expected_binding_id, expected_revision,
                         &expected_desired_state, &applied_revision) != ESP_OK) {
        AckCommand(command, "failed", "GUARD_RUNTIME_SYNC_FAILED");
        return;
    }
    AckCommand(command, "completed", "OK", "",
               ("{\"binding_id\":\"" + expected_binding_id +
                "\",\"runtime_revision\":" + std::to_string(applied_revision) +
                ",\"desired_runtime_state\":\"" + expected_desired_state + "\",\"running\":" +
                (guard_service_->IsRunning() ? "true" : "false") + "}").c_str());
    vTaskDelay(kActiveAckSettleDelay);
    ConnectChannel();
}
#endif

#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
void EidolonVoiceController::HandleGuardOwnerFaceProfileSyncCommand(
    const std::string& command_id, const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardOwnerFaceProfileSync;
    OwnerFaceEngine* engine =
        guard_service_ != nullptr ? guard_service_->owner_face_engine() : nullptr;
    if (engine == nullptr || device_control_uri_.empty()) {
        AckCommand(command, "failed", "OWNER_FACE_UNAVAILABLE");
        return;
    }

    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* profile_id = root ? cJSON_GetObjectItem(root, "profile_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "profile_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_state") : nullptr;
    int field_count = 0;
    int binding_count = 0;
    int profile_count = 0;
    int revision_count = 0;
    int desired_count = 0;
    bool only_known_fields = root != nullptr && cJSON_IsObject(root);
    for (const cJSON* item = root ? root->child : nullptr; item != nullptr; item = item->next) {
        ++field_count;
        const char* key = item->string ? item->string : "";
        if (strcmp(key, "binding_id") == 0) {
            ++binding_count;
        } else if (strcmp(key, "profile_id") == 0) {
            ++profile_count;
        } else if (strcmp(key, "profile_revision") == 0) {
            ++revision_count;
        } else if (strcmp(key, "desired_state") == 0) {
            ++desired_count;
        } else {
            only_known_fields = false;
        }
    }
    const bool integer_revision = cJSON_IsNumber(revision) && revision->valuedouble >= 1 &&
                                  revision->valuedouble <= UINT32_MAX &&
                                  std::floor(revision->valuedouble) == revision->valuedouble;
    const bool valid = only_known_fields && field_count == 4 && binding_count == 1 &&
                       profile_count == 1 && revision_count == 1 && desired_count == 1 &&
                       cJSON_IsString(binding_id) && binding_id->valuestring != nullptr &&
                       binding_id->valuestring[0] != '\0' && strlen(binding_id->valuestring) <= 64 &&
                       cJSON_IsString(profile_id) && profile_id->valuestring != nullptr &&
                       profile_id->valuestring[0] != '\0' && strlen(profile_id->valuestring) <= 64 &&
                       integer_revision && cJSON_IsString(desired) &&
                       desired->valuestring != nullptr &&
                       (strcmp(desired->valuestring, "active") == 0 ||
                        strcmp(desired->valuestring, "cleared") == 0);
    if (!valid) {
        cJSON_Delete(root);
        AckCommand(command, "failed", "INVALID_ARGUMENT");
        return;
    }
    OwnerFaceSyncRequest request = {
        .binding_id = binding_id->valuestring,
        .profile_id = profile_id->valuestring,
        .profile_revision = static_cast<uint32_t>(revision->valuedouble),
        .desired_state = desired->valuestring,
    };
    cJSON_Delete(root);

    const bool queued = engine->QueueSync(
        request, device_control_uri_, OperationalDeviceInstanceId(),
        [this, command_id](const OwnerFaceApplyResult& result) {
            cJSON* completion = cJSON_CreateObject();
            if (completion == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion allocation failed id=%s", command_id.c_str());
                return;
            }
            cJSON_AddStringToObject(completion, "command_id", command_id.c_str());
            cJSON_AddStringToObject(completion, "binding_id", result.request.binding_id.c_str());
            cJSON_AddStringToObject(completion, "profile_id", result.request.profile_id.c_str());
            cJSON_AddNumberToObject(completion, "profile_revision", result.request.profile_revision);
            cJSON_AddStringToObject(completion, "desired_state", result.request.desired_state.c_str());
            cJSON_AddBoolToObject(completion, "success", result.success);
            cJSON_AddStringToObject(completion, "code", result.code.c_str());
            cJSON_AddStringToObject(completion, "model_id", result.model_id.c_str());
            cJSON_AddStringToObject(completion, "preprocessing_version",
                                    result.preprocessing_version.c_str());
            cJSON_AddNumberToObject(completion, "template_count", result.template_count);
            char* encoded = cJSON_PrintUnformatted(completion);
            cJSON_Delete(completion);
            if (encoded == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion encoding failed id=%s", command_id.c_str());
                return;
            }
            Event ev;
            ev.type = EventType::OwnerFaceProfileCompleted;
            ev.payload = new (std::nothrow) std::string(encoded);
            cJSON_free(encoded);
            if (ev.payload == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion queue allocation failed id=%s",
                         command_id.c_str());
                return;
            }
            Enqueue(ev);
        });
    if (!queued) {
        AckCommand(command, "failed", "OWNER_FACE_BUSY");
    }
}

void EidolonVoiceController::DoOwnerFaceProfileCompleted(const std::string& payload)
{
    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* command_id = root ? cJSON_GetObjectItem(root, "command_id") : nullptr;
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* profile_id = root ? cJSON_GetObjectItem(root, "profile_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "profile_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_state") : nullptr;
    const cJSON* success = root ? cJSON_GetObjectItem(root, "success") : nullptr;
    const cJSON* code = root ? cJSON_GetObjectItem(root, "code") : nullptr;
    const cJSON* model = root ? cJSON_GetObjectItem(root, "model_id") : nullptr;
    const cJSON* preprocessing =
        root ? cJSON_GetObjectItem(root, "preprocessing_version") : nullptr;
    const cJSON* templates = root ? cJSON_GetObjectItem(root, "template_count") : nullptr;
    if (!cJSON_IsString(command_id) || !cJSON_IsString(binding_id) ||
        !cJSON_IsString(profile_id) || !cJSON_IsNumber(revision) ||
        !cJSON_IsString(desired) || !cJSON_IsBool(success) || !cJSON_IsString(code) ||
        !cJSON_IsString(model) || !cJSON_IsString(preprocessing) ||
        !cJSON_IsNumber(templates)) {
        ESP_LOGE(TAG, "Ignoring malformed Owner Face completion");
        cJSON_Delete(root);
        return;
    }
    ControlCommand command;
    command.id = command_id->valuestring;
    command.op = kControlOpGuardOwnerFaceProfileSync;
    if (!cJSON_IsTrue(success)) {
        AckCommand(command, "failed", code->valuestring);
        cJSON_Delete(root);
        return;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "binding_id", binding_id->valuestring);
    cJSON_AddStringToObject(result, "profile_id", profile_id->valuestring);
    cJSON_AddNumberToObject(result, "profile_revision", revision->valuedouble);
    cJSON_AddStringToObject(result, "applied_state", desired->valuestring);
    if (strcmp(desired->valuestring, "cleared") == 0) {
        cJSON_AddNullToObject(result, "model_id");
        cJSON_AddNullToObject(result, "preprocessing_version");
        cJSON_AddNumberToObject(result, "template_count", 0);
    } else {
        cJSON_AddStringToObject(result, "model_id", model->valuestring);
        cJSON_AddStringToObject(result, "preprocessing_version", preprocessing->valuestring);
        cJSON_AddNumberToObject(result, "template_count", templates->valueint);
    }
    char* encoded = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (encoded == nullptr) {
        AckCommand(command, "failed", "OWNER_FACE_RESULT_ENCODING_FAILED");
        cJSON_Delete(root);
        return;
    }
    AckCommand(command, "succeeded", "OK", "", encoded);
    cJSON_free(encoded);
    cJSON_Delete(root);
}
#endif

void EidolonVoiceController::HandleRoomJoinCommand(const std::string& command_id,
                                                   const std::string& payload)
{
    // An orchestrated wake arrives as room.join with a session_intent. Preserve
    // trusted presence/proactive intents through the JOIN token fetch; Channel
    // derives opening and idle behavior from the exact intent.
    // A user-initiated room.join may omit the payload or explicitly carry
    // "user_initiated"; in both cases pending stays empty.
    pending_session_intent_.clear();
    if (!payload.empty()) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root != nullptr) {
            const cJSON* intent = cJSON_GetObjectItem(root, kSessionIntentField);
            if (cJSON_IsString(intent) && intent->valuestring != nullptr) {
                const char* value = intent->valuestring;
                if (strcmp(value, kSessionIntentPresence) == 0 ||
                    strcmp(value, kSessionIntentProactive) == 0) {
                    pending_session_intent_ = value;
                } else if (strcmp(value, kSessionIntentUserInitiated) != 0) {
                    ESP_LOGW(TAG, "Ignoring unsupported session_intent=%s", value);
                }
            }
            cJSON_Delete(root);
        }
    }
    ESP_LOGI(TAG, "Control command -> join voice room (intent=%s)",
             pending_session_intent_.empty() ? "user" : pending_session_intent_.c_str());
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpRoomJoin;
    vTaskDelay(kRoomJoinSettleDelay);

    pending_room_join_command_ = command;
    pending_room_join_command_active_ = true;
    pending_room_join_generation_ = 0;
    esp_err_t err = DoJoinRoom();
    // One-shot: clear after the JOIN (incl. DoJoinRoom's internal connect-retry,
    // which re-fetches) so the intent never leaks into a later reconnect/refresh.
    pending_session_intent_.clear();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered room join failed: %s", esp_err_to_name(err));
        CompletePendingRoomJoinCommand("failed", JoinBlockedCode(), esp_err_to_name(err));
        return;
    }
    pending_room_join_generation_ = session_generation_;
    if (state_ == VoiceSessionState::InRoom) {
        char result[192];
        snprintf(result, sizeof(result),
                 "{\"status\":\"in_room\",\"room_name\":\"%s\",\"generation\":%lu}",
                 config_.session.room_name.c_str(),
                 static_cast<unsigned long>(session_generation_));
        CompletePendingRoomJoinCommand("completed", "OK", "", result);
    }
}

void EidolonVoiceController::HandlePlaybackStopCommand(const std::string& command_id,
                                                       const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> stop playback");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpPlaybackStop;

    esp_err_t flush_err = StopLocalPlayback("control_playback_stop");
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered playback stop failed: %s", esp_err_to_name(flush_err));
        AckCommand(command, "failed", "PLAYBACK_FLUSH_FAILED", esp_err_to_name(flush_err));
        return;
    }

    PublishClientAudioState(false);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandlePttTurnStatusCommand(const std::string& command_id,
                                                        const std::string& payload)
{
    std::string outcome;
    if (!payload.empty()) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root != nullptr) {
            const cJSON* item = cJSON_GetObjectItem(root, "outcome");
            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                outcome = item->valuestring;
            }
            cJSON_Delete(root);
        }
    }
    if (outcome.empty()) {
        outcome = "unknown";
    }
    ESP_LOGI(TAG, "PTT turn status outcome=%s", outcome.c_str());
    if (on_ptt_turn_status_) {
        on_ptt_turn_status_(outcome);
    }

    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpPttTurnStatus;
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleDeviceIdentifyCommand(const std::string& command_id,
                                                         const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> identify device");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpDeviceIdentify;

    if (!standby_) {
        ESP_LOGW(TAG, "Identify ignored outside standby");
        AckCommand(command, "failed", "IDENTIFY_REQUIRES_CONTROL_ROOM");
        return;
    }

    esp_err_t tone_err = PlayIdentifyFeedback();
    if (tone_err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered identify tone failed: %s", esp_err_to_name(tone_err));
        AckCommand(command, "failed", "IDENTIFY_TONE_FAILED", esp_err_to_name(tone_err));
        return;
    }

    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadLookAtCommand(const std::string& command_id,
                                                     const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadLookAt;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }

    float x = 0.0f, y = 0.0f;
    int speed = 500;    // 0..1000; motion layer default
    int ttl_ms = 0;     // 0 = hold until the next command; >0 arms return-home guardrail
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* jx = cJSON_GetObjectItem(root, "x");
        const cJSON* jy = cJSON_GetObjectItem(root, "y");
        const cJSON* jspeed = cJSON_GetObjectItem(root, "speed");
        const cJSON* jttl = cJSON_GetObjectItem(root, "ttl_ms");
        if (cJSON_IsNumber(jx)) x = static_cast<float>(jx->valuedouble);
        if (cJSON_IsNumber(jy)) y = static_cast<float>(jy->valuedouble);
        if (cJSON_IsNumber(jspeed)) speed = jspeed->valueint;
        if (cJSON_IsNumber(jttl)) ttl_ms = jttl->valueint;
        cJSON_Delete(root);
    }
    // Normalized inputs; the motion layer maps to the mechanical range and clamps.
    if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;
    if (speed < 0) speed = 0; else if (speed > 1000) speed = 1000;
    if (ttl_ms < 0) ttl_ms = 0;

    ESP_LOGI(TAG, "Control command -> head.look_at x=%.2f y=%.2f speed=%d ttl_ms=%d", x, y, speed, ttl_ms);
    board.HeadLookAt(x, y, speed, ttl_ms);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadHomeCommand(const std::string& command_id,
                                                   const std::string& /*payload*/)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadHome;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }
    ESP_LOGI(TAG, "Control command -> head.home");
    board.HeadHome();
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadGestureCommand(const std::string& command_id,
                                                      const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadGesture;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }

    std::string name;
    int times = 0, hold_ms = 0, return_ms = 0;
    float x = 0.0f, y = 0.0f;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* jn = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(jn) && jn->valuestring) name = jn->valuestring;
        const cJSON* jt = cJSON_GetObjectItem(root, "times");
        if (cJSON_IsNumber(jt)) times = jt->valueint;
        const cJSON* jh = cJSON_GetObjectItem(root, "hold_ms");
        if (cJSON_IsNumber(jh)) hold_ms = jh->valueint;
        const cJSON* jr = cJSON_GetObjectItem(root, "return_ms");
        if (cJSON_IsNumber(jr)) return_ms = jr->valueint;
        const cJSON* jx = cJSON_GetObjectItem(root, "x");
        if (cJSON_IsNumber(jx)) x = static_cast<float>(jx->valuedouble);
        const cJSON* jy = cJSON_GetObjectItem(root, "y");
        if (cJSON_IsNumber(jy)) y = static_cast<float>(jy->valuedouble);
        cJSON_Delete(root);
    }
    if (name.empty()) {
        AckCommand(command, "failed", "MISSING_GESTURE_NAME");
        return;
    }
    if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;

    ESP_LOGI(TAG, "Control command -> head.gesture %s", name.c_str());
    board.HeadGesture(name, times, x, y, hold_ms, return_ms);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleSafetyStopCommand(const std::string& command_id,
                                                     const std::string& /*payload*/)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpSafetyStop;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }
    ESP_LOGW(TAG, "Control command -> safety.stop (cut head torque)");
    board.HeadStop();
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandlePresenceSetCommand(const std::string& command_id,
                                                      const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpPresenceSet;

    // Guard -> hub -> body fan-out payload. Only `state` drives a reaction;
    // guard_epoch/correlation_id come along for the contract but the device is not
    // authoritative over them (the Hub is). action_id is echoed back in the result.
    // The Hub delivers state="awake" (owner present/candidate) or "warm" (absent).
    std::string state, action_id;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* js = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(js) && js->valuestring) state = js->valuestring;
        const cJSON* ja = cJSON_GetObjectItem(root, "action_id");
        if (cJSON_IsString(ja) && ja->valuestring) action_id = ja->valuestring;
        cJSON_Delete(root);
    }
    if (state.empty()) {
        AckCommand(command, "failed", "MISSING_PRESENCE_STATE");
        return;
    }

    // Render owner presence as a device-local reflex reaction — a generic Hub signal,
    // device-specific behavior. Each effector is independently safe (no-op if absent),
    // so no HasHeadMotion gate: sound + avatar work even without the servo body, and
    // the body serializes its own motion (safety.stop > gesture > idle).
    //   awake (owner present/candidate): tech chime + cute head wobble + RGB marquee + happy face
    //   warm  (owner absent):            settle head down + dim the ring + sleepy face
    auto& board = Board::GetInstance();
    bool applied = false;
    if (state == "awake") {
        PlayStartupCue();
        board.HeadGesture("wake_wobble", 0, 0.0f, 0.0f, 0, 0);
        board.RgbEffect("wake");
        board.AvatarExpress("happy", 4000);
        applied = true;
    } else if (state == "warm") {
        board.HeadGesture("droop", 0, 0.0f, 0.0f, 0, 0);
        board.RgbEffect("off");
        board.AvatarExpress("sleepy", 2000);
        applied = true;
    } else {
        ESP_LOGW(TAG, "presence.set unknown state=%s", state.c_str());
    }
    ESP_LOGI(TAG, "Control command -> body.presence.set state=%s applied=%d",
             state.c_str(), applied ? 1 : 0);

    // Result matches the SDK result_schema {action_id, state, applied}.
    char result[192];
    snprintf(result, sizeof(result),
             "{\"action_id\":\"%s\",\"state\":\"%s\",\"applied\":%s}",
             action_id.c_str(), state.c_str(), applied ? "true" : "false");
    AckCommand(command, "completed", "OK", "", result);
}

#if CONFIG_EIDOLON_GUARD_SERVICE
void EidolonVoiceController::HandleDeviceRollCallCommand(const std::string& command_id,
                                                         const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> Guard roll call");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpDeviceRollCall;

    if (!standby_) {
        AckCommand(command, "failed", "ROLL_CALL_REQUIRES_CONTROL_ROOM");
        return;
    }
    const esp_err_t feedback_err = PlayRollCallFeedback();
    if (feedback_err != ESP_OK) {
        AckCommand(command, "failed", "ROLL_CALL_PLAYBACK_FAILED",
                   esp_err_to_name(feedback_err));
        return;
    }
    AckCommand(command, "completed", "OK", "", "{\"played\":true}");
}
#endif

#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
void EidolonVoiceController::HandleGuardVisionBenchmarkCommand(const std::string& command_id,
                                                                const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardVisionBenchmark;

    if (!standby_) {
        AckCommand(command, "failed", "VISION_PROBE_REQUIRES_CONTROL_ROOM");
        return;
    }

    int sample_count = 30;
    int interval_ms = 300;
    cJSON* root = payload.empty() ? nullptr : cJSON_Parse(payload.c_str());
    if (!payload.empty() && root == nullptr) {
        AckCommand(command, "failed", "INVALID_ARGUMENT", "invalid JSON payload");
        return;
    }
    if (root != nullptr) {
        const cJSON* samples = cJSON_GetObjectItem(root, "sample_count");
        const cJSON* interval = cJSON_GetObjectItem(root, "interval_ms");
        if ((samples != nullptr && !cJSON_IsNumber(samples)) ||
            (interval != nullptr && !cJSON_IsNumber(interval))) {
            cJSON_Delete(root);
            AckCommand(command, "failed", "INVALID_ARGUMENT", "sample_count and interval_ms must be integers");
            return;
        }
        if (samples != nullptr) {
            sample_count = samples->valueint;
        }
        if (interval != nullptr) {
            interval_ms = interval->valueint;
        }
        cJSON_Delete(root);
    }
    if (sample_count < 10 || sample_count > 120 || interval_ms < 100 || interval_ms > 2000) {
        AckCommand(command, "failed", "INVALID_ARGUMENT", "sample_count=10..120, interval_ms=100..2000");
        return;
    }

    Camera* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        AckCommand(command, "failed", "CAMERA_UNAVAILABLE");
        return;
    }

    ESP_LOGI(TAG, "Control command -> guard vision benchmark samples=%d interval_ms=%d",
             sample_count, interval_ms);
    const std::string result = GuardVisionBenchmark::Run(*camera, sample_count, interval_ms);
    AckCommand(command, "completed", "OK", "", result.c_str());
}
#endif

void EidolonVoiceController::HandleIdleTimeoutCommand()
{
    // Retained for the legacy idle path; routed through the unified handler.
    HandleSessionEnd(EndReason::IdleNormalEnd);
}

void EidolonVoiceController::HandleSessionEnd(EndReason reason)
{
    ResetPresenceManagedSession();
    static const char* kReasonNames[] = {"none",
                                         kSessionEndIdleNormal,
                                         kSessionEndProactiveDone,
                                         kSessionEndUserLeft,
                                         kSessionEndSuperseded,
                                         kSessionEndError};
    const char* reason_name = kReasonNames[static_cast<int>(reason)];
    // Record the reason so the UI can show "已结束待命" / error chrome instead of an
    // unexplained return to JOIN, then tear the voice room down gracefully and
    // fall back to the (always-on) channel for reachability. The device is
    // still reachable on control even after an error, so we do NOT force the
    // Error connection state; the reason drives the UI text orthogonally (§3.2).
    last_end_reason_ = reason;
    ESP_LOGI(TAG,
             "[lifecycle] session_end reason=%s; returning to standby (gen=%lu room=%s)",
             reason_name, static_cast<unsigned long>(session_generation_),
             config_.session.room_name.c_str());

    // The conversation ended, not the channel. Nothing is torn down and nothing
    // is reconnected: the device stays exactly where it is, reachable, and the
    // only thing that changed is that no one is listening any more.
    CloseConversationAudio();
    standby_ = true;
    current_conversation_id_.clear();
    conversation_confirmed_ = false;
    SetState(StateForConfig(config_), "session_end");
}

// ============================ Lifecycle handlers ============================

void EidolonVoiceController::DoActivation()
{
    channel_recovery_.OnActivation();
    // Activation rebuilds the device's whole operational picture, so any
    // internal-memory ceiling measured before it describes a device that no
    // longer exists.
    session_.ForgetInternalMemoryCeiling();
    memory_ceiling_announced_ = false;
    // Wire the SDK callbacks to post events so all state mutation stays on the loop.
    session_.SetOnStateChanged([this](LiveKitConnectionState s, uint32_t generation) {
        Event ev;
        ev.type = EventType::LiveKitState;
        ev.lk_state = s;
        // LiveKitSession binds this value when the room is created. Never read
        // the controller's newer generation from the SDK callback task: a late
        // teardown must retain ownership by the room that emitted it.
        ev.generation = generation;
        Enqueue(ev);
    });
    session_.SetOnDeviceEvent([this](const std::string& payload, uint32_t generation) {
        Event ev;
        ev.type = EventType::DeviceEvent;
        ev.generation = generation;
        ev.payload = new (std::nothrow) std::string(payload);
        if (ev.payload != nullptr) {
            Enqueue(ev);
        }
    });
    session_.SetOnControlCommand([this](const std::string& payload) {
        Event ev;
        ev.type = EventType::ControlCommand;
        ev.payload = new std::string(payload);
        Enqueue(ev);
    });
    session_.SetOnSessionControl([this](const std::string& payload) {
        Event ev;
        ev.type = EventType::SessionControl;
        ev.payload = new std::string(payload);
        Enqueue(ev);
    });

    if (LoadStoredConfig() != ESP_OK) {
        return;
    }

    if (config_.status == HubConfigStatus::PendingApproval ||
        config_.status == HubConfigStatus::WaitingBinding) {
        // HubActivator just performed the first handoff. Avoid immediately
        // repeating it; the normal bounded poll owns the next attempt.
        ScheduleOnboardingPoll();
    } else if (!HasActiveConfig()) {
        const esp_err_t refresh_err = RefreshHubConfig();
        if (refresh_err != ESP_OK) {
            ESP_LOGW(TAG, "Initial Hub config refresh failed: %s", esp_err_to_name(refresh_err));
        }
    }
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        const esp_err_t guard_err = SyncGuardRuntime("activation");
        if (guard_err != ESP_OK && guard_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Initial Guard runtime sync failed: %s", esp_err_to_name(guard_err));
        }
    }
#endif

    // PTT and half-duplex: land in the ready state on the lightweight control
    // room — entering the voice room (which plays the welcome + opens the mic) is
    // an explicit "Start session", so the device never boots straight into an open
    // session. Only full-duplex (always-on companion) may auto-join the voice room
    // on activation when the board opts in.
#if CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION
    if (IsFullDuplex() && HasActiveConfig()) {
        DoJoinRoom();
        return;
    }
#endif
    if (HasChannelConfig()) {
        ConnectChannel();
    }
}

void EidolonVoiceController::DoNetworkLost()
{
    ESP_LOGW(TAG,
             "[lifecycle] network_lost executing state=%s room_kind=%s gen=%lu "
             "connected=%d room=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             config_.session.room_name.c_str());
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    if (radar_presence_known_) {
        if (!radar_presence_dirty_) {
            radar_pending_observation_ =
                AmbientPresenceObservation::Snapshot;
        }
        radar_presence_dirty_ = true;
    }
#endif
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    ambient_presence_registry_.DeactivateAll();
#endif
    if (ambient_presence_timer_ != nullptr) {
        esp_timer_stop(ambient_presence_timer_);
    }
    CloseConversationAudio();
    channel_recovery_.OnNetworkLost();
    SetOperationalReady(false, "network_lost");
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    if (onboarding_poll_timer_ != nullptr) {
        esp_timer_stop(onboarding_poll_timer_);
    }
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        // Presence facts are meaningful only while a signed Guard runtime is
        // current. Do not retain local observations across a network boundary.
        ClearGuardPresenceRuntime();
        guard_service_->Stop("network_lost");
    }
#endif
    standby_ = false;
    session_.Disconnect();
    // Supersede so late callbacks from the dropped connection don't resurrect a
    // stale state once the network returns and we reconnect.
    MarkSessionSuperseded("network_lost");
    SetState(StateForConfig(config_), "network_lost");
}

bool EidolonVoiceController::DoCommissioningQuiesce()
{
    ESP_LOGI(TAG,
             "[commissioning] quiescing operational runtime before RadioLease");
    // Commissioning is an explicit product-mode boundary, not a transient
    // network interruption. Do not resurrect the old conversation after the
    // Owner finishes setup and the operational channel reconnects.
    current_conversation_id_.clear();
    conversation_confirmed_ = false;
    last_end_reason_ = EndReason::UserLeft;
    DoNetworkLost();
    const bool quiesced = !session_.HasRoom() && !session_.IsConnected();
    ESP_LOGI(TAG, "[commissioning] operational runtime quiesced=%d",
             quiesced ? 1 : 0);
    return quiesced;
}

void EidolonVoiceController::DoNetworkRestored()
{
    ESP_LOGI(TAG, "Network restored; refreshing Hub discovery/config");
    channel_recovery_.OnNetworkRestored();
    // Going offline and back tears down and rebuilds sockets, TLS sessions and
    // buffers, which is exactly the kind of churn that can hand back the
    // contiguous internal block a session needs. Worth one more look.
    session_.ForgetInternalMemoryCeiling();
    memory_ceiling_announced_ = false;
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }

    if (device_control_uri_.empty() && LoadStoredConfig() != ESP_OK) {
        ESP_LOGW(TAG, "Network restored but no stored Hub config is available");
        return;
    }

    esp_err_t err = RediscoverHub();
    if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGW(TAG, "Network restore stopped: Hub rejected device identity");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hub rediscovery after network restore failed: %s", esp_err_to_name(err));
        err = RefreshHubConfig();
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGW(TAG, "Network restore stopped: Hub rejected device identity");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hub config refresh after network restore failed: %s", esp_err_to_name(err));
        ScheduleChannelReconnect("network_restore_refresh_failed");
        return;
    }

#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        const esp_err_t guard_err = SyncGuardRuntime("network_restore");
        if (guard_err != ESP_OK && guard_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Guard runtime sync after network restore failed: %s", esp_err_to_name(guard_err));
        }
    }
#endif

    if (HasChannelConfig()) {
        err = ConnectChannel();
        if (err != ESP_OK) {
            ScheduleChannelReconnect("network_restore_connect_failed");
        }
        return;
    }

    SetState(StateForConfig(config_), "network_restored");
}

esp_err_t EidolonVoiceController::DoJoinRoom()
{
    const bool presence_initiated =
        pending_session_intent_ == kSessionIntentPresence &&
        (presence_wake_phase_ == PresenceWakePhase::OwnerRecognized ||
         presence_managed_voice_session_);
    if (!presence_initiated) {
        ResetPresenceManagedSession();
    }
    // Somebody (or presence) is asking for a conversation now. Never answer that
    // with a refusal cached from an earlier attempt: measure the heap again and
    // let the attempt fail on today's numbers if it must.
    session_.ForgetInternalMemoryCeiling();
    memory_ceiling_announced_ = false;
    ESP_LOGI(TAG,
             "[lifecycle] join executing state=%s room_kind=%s gen=%lu standby=%d "
             "connected=%d room=%s intent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), standby_ ? 1 : 0,
             session_.IsConnected() ? 1 : 0, config_.session.room_name.c_str(),
             pending_session_intent_.empty() ? "user" : pending_session_intent_.c_str());
    if (state_ == VoiceSessionState::Connecting || state_ == VoiceSessionState::Opening ||
        state_ == VoiceSessionState::Reconnecting) {
        ESP_LOGI(TAG, "[lifecycle] join ignored while session is already transitioning state=%s",
                 VoiceStateName(state_));
        return ESP_ERR_INVALID_STATE;
    }
    if (state_ == VoiceSessionState::InRoom) {
        ESP_LOGI(TAG, "[lifecycle] join ignored: already in voice room=%s gen=%lu",
                 config_.session.room_name.c_str(),
                 static_cast<unsigned long>(session_generation_));
        return ESP_OK;
    }

    if (!config_.session.usable()) {
        if (LoadStoredConfig() != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    // Ask Hub for a current provider binding before every JOIN. The provider
    // owns room/token rotation semantics; the device treats the binding as
    // opaque until it validates the versioned LiveKit payload. RAM-only
    // (persist=false) avoids NVS wear. Done before moving to Connecting so
    // RefreshHubConfig's internal SetState(ConfigReady) stays a no-op.
    esp_err_t refresh_err = RefreshHubConfig(/*persist=*/false);
    if (refresh_err == ESP_ERR_NOT_ALLOWED) {
        // Hub revoked this identity; RefreshHubConfig already surfaced
        // Unauthorized. Don't attempt to join on a rejected key.
        return refresh_err;
    }
    if (refresh_err != ESP_OK) {
        // Hub unreachable: an unexpired cached room+token can provide bounded
        // recovery. HasActiveConfig below rejects it after Provider expiry.
        ESP_LOGW(TAG, "Join: Hub config refresh failed (%s); using cached room=%s",
                 esp_err_to_name(refresh_err), config_.session.room_name.c_str());
    }
    if (!HasActiveConfig()) {
        ESP_LOGW(TAG, "Join blocked: config status=%s",
                 HubConfigStatusToString(config_.status));
        SetState(StateForConfig(config_), "join_blocked_inactive");
        return refresh_err != ESP_OK ? refresh_err : ESP_ERR_INVALID_STATE;
    }

    // Fresh session: drop any end reason from the previous conversation so the
    // connecting/ready chrome doesn't show a stale "已结束".
    last_end_reason_ = EndReason::None;
    current_conversation_id_ = NewConversationId();
    conversation_confirmed_ = false;

    // The channel is normally already up and the conversation starts by saying
    // so. Connecting here is the exception — a device that was knocked offline
    // and has not got back yet — and only then is there anything to wait for.
    if (!session_.IsConnected()) {
        standby_ = true;
        SetState(VoiceSessionState::Connecting, "join_requested");
        esp_err_t connect_err = ConnectChannel();
        if (connect_err != ESP_OK) {
            ESP_LOGW(TAG, "Join: channel connect failed (%s), refreshing Hub token",
                     esp_err_to_name(connect_err));
            if (RefreshHubConfig(/*persist=*/false) == ESP_OK) {
                connect_err = ConnectChannel();
            }
        }
        if (connect_err != ESP_OK) {
            current_conversation_id_.clear();
            conversation_confirmed_ = false;
            SetState(VoiceSessionState::Error, "join_connect_failed");
            ScheduleChannelReconnect("channel_connect_sync_failed");
            return connect_err;
        }
        // ConnectChannel completes asynchronously. The Connected event publishes
        // this same desired conversation; no request is sent against a channel
        // that is not ready yet.
        return ESP_OK;
    }

    esp_err_t err = PublishSessionRequest(kSessionOpenType, current_conversation_id_);
    if (err != ESP_OK) {
        // The request never left the device, so nobody is coming. Say so rather
        // than sit in a conversation the server was never asked for.
        current_conversation_id_.clear();
        conversation_confirmed_ = false;
        SetState(VoiceSessionState::Error, "join_request_failed");
        return err;
    }
    // The request is only desired state. Audio and the active UI begin after the
    // agent has successfully started and confirms this exact conversation id.
    SetState(VoiceSessionState::Opening, "session_open_sent");
    return ESP_OK;
}

esp_err_t EidolonVoiceController::ConnectChannel()
{
    if (!channel_recovery_.network_available() || !HasChannelConfig()) {
        return ESP_ERR_INVALID_STATE;
    }

    // One channel, so there is no room to choose — only which credential is
    // trustworthy yet, which is still open before registration completes.
    const RoomConfig* runtime_fallback = nullptr;
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (has_guard_control_config_) {
        runtime_fallback = &guard_control_config_;
    }
#endif
    Esp32HubConfig control_config = BuildChannelConnectionConfig(config_, runtime_fallback);

    // Retire any old room before assigning the next generation. Disconnect is
    // synchronous in LiveKitSession; the explicit supersede generation also
    // makes callbacks already in flight stale before the new attempt begins.
    if (session_.HasRoom()) {
        MarkSessionSuperseded("control_attempt_replace");
        session_.Disconnect();
    }
    standby_ = true;
    const uint32_t control_generation = BeginSessionGeneration("control");
    channel_recovery_.OnAttemptStarted();
    esp_err_t err = session_.Connect(control_config, control_generation);
    if (err != ESP_OK) {
        // Keep standby_ true: it denotes the plane owned by this generation,
        // not connection health. Superseding drops any Connecting/Disconnected
        // callback queued before the synchronous failure was returned.
        channel_recovery_.OnDisconnected();
        SetOperationalReady(false, "control_connect_sync_failed");
        MarkSessionSuperseded("control_connect_sync_failed");
        ESP_LOGW(TAG, "Connect channel failed: %s", esp_err_to_name(err));
    }
    // ServerUnreachable remains visible until LiveKit actually reports Connected.
    // A desired conversation is orthogonal to transport health: keep its
    // opening/reconnecting projection while the persistent channel comes back.
    if (state_ != VoiceSessionState::ServerUnreachable) {
        if (current_conversation_id_.empty()) {
            SetState(StateForConfig(config_), "control_connect");
        } else if (state_ == VoiceSessionState::Reconnecting) {
            SetState(VoiceSessionState::Reconnecting,
                     "control_connect_for_conversation");
        } else {
            SetState(VoiceSessionState::Connecting,
                     "control_connect_for_conversation");
        }
    }
    if (err == ESP_OK) {
        ArmConnectWatchdog();
    } else {
        ScheduleChannelReconnect("control_connect_sync_failed");
    }
    return err;
}

#if CONFIG_EIDOLON_GUARD_SERVICE
esp_err_t EidolonVoiceController::SyncGuardRuntime(const char* reason,
                                                    const std::string* expected_binding_id,
                                                    uint32_t expected_runtime_revision,
                                                    const std::string* expected_desired_state,
                                                    uint32_t* applied_runtime_revision)
{
    if (guard_service_ == nullptr || device_control_uri_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    GuardRuntimeHubConfig runtime;
    HubConfigClient client;
    const esp_err_t err =
        client.FetchGuardRuntime(device_control_uri_, OperationalDeviceInstanceId(), runtime);
    if (err == ESP_ERR_NOT_FOUND) {
        ClearGuardPresenceRuntime();
        guard_service_->Stop("guard_binding_missing");
        has_guard_control_config_ = false;
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if ((expected_binding_id && runtime.binding_id != *expected_binding_id) ||
        (expected_runtime_revision > runtime.runtime_revision) ||
        (expected_desired_state && runtime.desired_runtime_state != *expected_desired_state)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (applied_runtime_revision != nullptr) {
        *applied_runtime_revision = runtime.runtime_revision;
    }
    has_guard_control_config_ = true;
    guard_control_config_ = runtime.control;
    if (runtime.desired_runtime_state == "stopped") {
        guard_service_->Stop(reason);
        ClearGuardPresenceRuntime();
        has_guard_control_config_ = false;
        guard_control_config_ = RoomConfig{};
        return ESP_OK;
    }
    GuardRuntimeConfig config;
    config.sample_interval_ms = runtime.sample_interval_ms;
    config.preview_interval_ms = runtime.preview_interval_ms;
    config.motion_threshold = runtime.motion_threshold;
    config.motion_clear_threshold = runtime.motion_clear_threshold;
    config.candidate_debounce_ms = runtime.candidate_debounce_ms;
    config.absence_timeout_ms = runtime.absence_timeout_ms;
    config.consecutive_capture_failures = runtime.consecutive_capture_failures;
    config.owner_face_interval_ms = runtime.owner_face_interval_ms;
    config.owner_presence_enter_ms = runtime.owner_presence_enter_ms;
    config.owner_presence_exit_ms = runtime.owner_presence_exit_ms;
    config.owner_presence_heartbeat_ms = runtime.owner_presence_heartbeat_ms;
    config.owner_presence_lease_ms = runtime.owner_presence_lease_ms;
    ConfigureGuardPresenceRuntime(runtime);
    return guard_service_->Start(config, reason) ? ESP_OK : ESP_FAIL;
}

void EidolonVoiceController::ConfigureGuardPresenceRuntime(const GuardRuntimeHubConfig& runtime)
{
    if (guard_service_ == nullptr) {
        return;
    }
    ++guard_runtime_generation_;
    pending_guard_presence_payloads_.clear();
    const uint32_t boot_nonce = esp_random();
    guard_presence_adapter_.Configure({
        .guard_companion_id = runtime.guard_companion_id,
        .device_id = OperationalDeviceInstanceId(),
        .runtime_revision = runtime.runtime_revision,
        .candidate_debounce_ms = runtime.candidate_debounce_ms,
        .boot_nonce = boot_nonce,
    });
    owner_presence_adapter_.Configure({
        .guard_companion_id = runtime.guard_companion_id,
        .device_id = OperationalDeviceInstanceId(),
        .boot_nonce = boot_nonce,
    });
    const uint32_t generation = guard_runtime_generation_;
    guard_service_->SetObservationCallback([this, generation](const GuardObservation& observation) {
        Event ev;
        ev.type = EventType::GuardObservation;
        ev.generation = generation;
        ev.guard_observation = observation;
        Enqueue(ev);
    });
    guard_service_->SetOwnerPresenceCallback(
        [this, generation](const OwnerPresenceObservation& observation) {
            Event ev;
            ev.type = EventType::OwnerPresence;
            ev.generation = generation;
            ev.owner_presence_observation = observation;
            Enqueue(ev);
        });
}

void EidolonVoiceController::ClearGuardPresenceRuntime()
{
    ++guard_runtime_generation_;
    pending_guard_presence_payloads_.clear();
    guard_presence_adapter_.Clear();
    owner_presence_adapter_.Clear();
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        guard_service_->SetObservationCallback({});
        guard_service_->SetOwnerPresenceCallback({});
    }
#endif
}

void EidolonVoiceController::DoGuardObservation(const GuardObservation& observation,
                                                 uint32_t runtime_generation)
{
    if (runtime_generation != guard_runtime_generation_) {
        ESP_LOGD(TAG, "Dropped stale Guard observation epoch=%lu generation=%lu",
                 static_cast<unsigned long>(observation.epoch),
                 static_cast<unsigned long>(runtime_generation));
        return;
    }
    auto payload = guard_presence_adapter_.Build(
        observation, GuardEventTimestampMs(observation.now_ms));
    if (!payload.has_value()) {
        return;
    }
    if (pending_guard_presence_payloads_.size() >= kMaxPendingGuardPresenceEvents) {
        ESP_LOGW(TAG, "Dropped Guard fact: volatile queue is full epoch=%lu state=%s",
                 static_cast<unsigned long>(observation.epoch), GuardStateName(observation.state));
        return;
    }
    pending_guard_presence_payloads_.push_back(std::move(*payload));
    FlushPendingGuardPresence();
}

void EidolonVoiceController::DoOwnerPresence(
    const OwnerPresenceObservation& observation, uint32_t runtime_generation)
{
    if (runtime_generation != guard_runtime_generation_) {
        ESP_LOGD(TAG, "Dropped stale Owner Presence fact epoch=%lu generation=%lu",
                 static_cast<unsigned long>(observation.epoch),
                 static_cast<unsigned long>(runtime_generation));
        return;
    }
    auto payload = owner_presence_adapter_.Build(
        observation, GuardEventTimestampMs(observation.now_ms));
    if (!payload.has_value()) {
        return;
    }
    if (pending_guard_presence_payloads_.size() >= kMaxPendingGuardPresenceEvents) {
        ESP_LOGW(TAG, "Dropped Owner Presence fact: volatile queue is full epoch=%lu",
                 static_cast<unsigned long>(observation.epoch));
        return;
    }
    pending_guard_presence_payloads_.push_back(std::move(*payload));
    FlushPendingGuardPresence();

#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    if (observation.fact == OwnerPresenceFact::Present) {
        PublishPendingOwnerConfirmations(observation);
    } else if (observation.fact == OwnerPresenceFact::Absent) {
        // Keep active radar assertions armed. The next local owner appearance
        // can confirm them again even when the radar never returned to vacant.
        ambient_presence_registry_.OnOwnerAbsent();
    }
#endif

#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    // The signed Guard fact remains the durable control-plane source of truth.
    // Mirror only its privacy-bounded present/absent lease onto the generic
    // owner-scoped event bus so companion devices can manage an open session.
    const auto epoch_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (epoch_now < 1'700'000'000'000LL ||
        observation.fact_profile_revision == 0 ||
        observation.sequence == 0) {
        return;
    }
    const bool present = observation.fact == OwnerPresenceFact::Present;
    char lifecycle_payload[256] = {};
    const int written = std::snprintf(
        lifecycle_payload, sizeof(lifecycle_payload),
        "{\"state\":\"%s\",\"profile_revision\":%lu,\"guard_epoch\":%lu,"
        "\"presence_sequence\":%lu,\"lease_ms\":%lu,"
        "\"evidence\":\"face_gated_person_presence\","
        "\"raw_retention\":\"none\"}",
        present ? "present" : "absent",
        static_cast<unsigned long>(observation.fact_profile_revision),
        static_cast<unsigned long>(observation.epoch),
        static_cast<unsigned long>(observation.sequence),
        static_cast<unsigned long>(present ? observation.lease_ms : 0));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(lifecycle_payload)) {
        return;
    }
    const uint64_t monotonic_ms =
        static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const std::string flow_id =
        std::string("owner-presence-") + OperationalDeviceInstanceId() + "-" +
        std::to_string(observation.epoch);
    const std::string event_json = BuildDeviceEventJson({
        .event_id = MakeDeviceEventId("evt-owner-life", monotonic_ms, esp_random()),
        .flow_id = flow_id,
        .causation_id = "",
        .type = kIdentityOwnerPresenceChangedType,
        .source_device_id = OperationalDeviceInstanceId(),
        .source_component = "owner_presence",
        .occurred_at_ms = static_cast<uint64_t>(epoch_now),
        .expires_at_ms = static_cast<uint64_t>(epoch_now) +
                         DeviceEventBus::kMaxTtlMs,
        .payload_json = lifecycle_payload,
    });
    if (!event_json.empty()) {
        DoPublishDeviceEvent(event_json);
    }
#endif
}

void EidolonVoiceController::FlushPendingGuardPresence()
{
    if (!has_guard_control_config_ || !standby_ || !session_.IsConnected()) {
        return;
    }
    while (!pending_guard_presence_payloads_.empty()) {
        const esp_err_t err = session_.PublishData(kControlTopic,
                                                   pending_guard_presence_payloads_.front());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Guard fact publish deferred: %s", esp_err_to_name(err));
            return;
        }
        pending_guard_presence_payloads_.pop_front();
    }
}

uint64_t EidolonVoiceController::GuardEventTimestampMs(uint64_t monotonic_ms)
{
    const std::time_t seconds = std::time(nullptr);
    // The signed config route intentionally supports pre-SNTP devices. Preserve
    // a useful ordering timestamp in that case, but never treat it as wall clock.
    if (seconds >= 1'700'000'000) {
        return static_cast<uint64_t>(seconds) * 1000ULL;
    }
    return monotonic_ms;
}
#endif

esp_err_t EidolonVoiceController::DoLeaveRoom()
{
    ResetPresenceManagedSession();
    bool playback_recent = PlaybackActiveRecently();
    ESP_LOGI(TAG,
             "[lifecycle] leave executing state=%s room_kind=%s gen=%lu connected=%d "
             "standby=%d room=%s ptt_active=%d tail=%d "
             "audio_pub=%d playback_recent=%d local_playback=%d agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             standby_ ? 1 : 0, config_.session.room_name.c_str(), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, audio_publisher_active_ ? 1 : 0,
             playback_recent ? 1 : 0, local_playback_ui_active_ ? 1 : 0,
             AgentPhaseName(agent_phase_));
    if (!standby_) {
        esp_err_t stop_err = StopLocalPlayback("leave_room");
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Leave-room playback stop skipped: %s", esp_err_to_name(stop_err));
        }
    }
    CloseConversationAudio();
    // Leaving a conversation is now something the device says, not somewhere it
    // goes. Failing to say it is not worth dropping the channel over: the
    // server ends an unattended session on its own, and staying reachable is
    // what lets the next conversation start at all.
    esp_err_t err = ESP_OK;
    if (!current_conversation_id_.empty()) {
        err = PublishSessionRequest(kSessionCloseType, current_conversation_id_);
    }
    current_conversation_id_.clear();
    conversation_confirmed_ = false;
    last_end_reason_ = EndReason::UserLeft;
    standby_ = true;
    if (state_ != VoiceSessionState::Idle) {
        SetState(StateForConfig(config_), "leave_room");
    }
    ESP_LOGI(TAG, "[lifecycle] leave complete err=%s state=%s gen=%lu connected=%d",
             esp_err_to_name(err), VoiceStateName(state_),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0);
    return err;
}

// ============================ Audio state publisher ============================

void EidolonVoiceController::OpenConversationAudio()
{
    // A conversation is the only reason this device listens, so the microphone
    // opens here and nowhere else. How far it opens is the turn-taking profile's
    // business — push-to-talk, half duplex and full duplex disagree — which
    // PublishClientAudioState already decides in one place; applying it now
    // rather than waiting for the first telemetry tick means the mic is open
    // because the conversation started, not because a timer fired.
    StartAudioStatePublisher();
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::CloseConversationAudio()
{
    // Unconditional, unlike the telemetry above it: the channel outlives the
    // conversation now, so a gate left open is a microphone running for as long
    // as the device sits in standby — which is most of its life. Whether anyone
    // can still be told about it is a separate question from whether it closes.
    StopAudioStatePublisher();
    eidolon_livekit_board_set_capture_enabled(false);
}

void EidolonVoiceController::StartAudioStatePublisher()
{
    if (audio_publisher_active_) {
        return;
    }
    audio_state_seq_ = 0;
    audio_state_sent_ = false;
    local_playback_ui_active_ = false;
    audio_publisher_active_ = true;
    if (audio_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::AudioTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_audio";
        if (esp_timer_create(&args, &audio_timer_) != ESP_OK) {
            audio_timer_ = nullptr;
            audio_publisher_active_ = false;
            ESP_LOGW(TAG, "Failed to create audio state timer");
            return;
        }
    }
    esp_timer_start_periodic(audio_timer_, kAudioTickIntervalUs);
}

void EidolonVoiceController::StopAudioStatePublisher()
{
    if (!audio_publisher_active_) {
        return;
    }
    audio_publisher_active_ = false;
    if (audio_timer_ != nullptr) {
        esp_timer_stop(audio_timer_);
    }
    if (ptt_mode_) {
        CancelPttReleaseTail();
        ptt_active_ = false;
    }
    // Emit a final closed-mic state (matches the old publisher's exit behavior).
    if (session_.IsConnected() && !standby_) {
        PublishClientAudioState(false);
    }
}

void EidolonVoiceController::AudioTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::AudioTick;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoAudioTick()
{
    if (!audio_publisher_active_ || standby_ || !session_.IsConnected()) {
        return;
    }
    CheckOwnerPresenceLease();
    if (!audio_publisher_active_ || standby_ || !session_.IsConnected()) {
        return;
    }
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::PublishClientAudioState(bool playback_active)
{
    char payload[320];
    int64_t now_us = esp_timer_get_time();
    uint32_t client_ts_ms = static_cast<uint32_t>(now_us / 1000);

    bool ptt_held = ptt_mode_ && ptt_active_;
    bool mic_muted;
    bool capture_on;
    if (ptt_mode_) {
        // Push-to-talk: the mic is open while held and during the short release
        // tail. Closed otherwise, so playback is not recorded and the ptt=false
        // edge remains the explicit "I'm done" turn boundary.
        capture_on = mic_enabled_ && ptt_active_;
        mic_muted = !capture_on;
    } else if (half_duplex_mode_) {
        // Half-duplex: auto open-mic, but CLOSED while the agent is speaking (and
        // the playback hangover). This board has no device AEC, so muting the mic
        // during playback is what stops it recording its own output / self-
        // interrupting. When the agent is idle the mic is open and the server EOT
        // decides the turn — no barge-in.
        capture_on = mic_enabled_ && !playback_active;
        mic_muted = !capture_on;
    } else {
        // Full-duplex: keep capture open during playback so device-side AEC can
        // support barge-in.
        capture_on = mic_enabled_;
        mic_muted = !mic_enabled_;
    }

    // Physically gate the capture path to match so no unwanted audio reaches the
    // channel.
    eidolon_livekit_board_set_capture_enabled(capture_on);
    uint32_t capture_rms_ppm = eidolon_livekit_board_recent_capture_rms_ppm();
    if (mic_muted) {
        capture_rms_ppm = 0;
    }
    uint32_t playback_rms_ppm = eidolon_livekit_board_recent_playback_rms_ppm();

    bool state_changed = !audio_state_sent_ ||
                         playback_active != last_audio_playback_active_ ||
                         mic_muted != last_audio_mic_muted_ ||
                         ptt_held != last_audio_ptt_;
    UpdateLocalPlaybackPhase(playback_active);
    // Fast poll, throttled heartbeat: nothing changed and the heartbeat isn't due
    // yet, so don't emit a packet (last_audio_* already equals the current state).
    if (!state_changed && (now_us - last_audio_publish_us_) < kAudioStateHeartbeatUs) {
        return;
    }
    uint32_t seq = ++audio_state_seq_;
    unsigned long capture_rms_whole =
        static_cast<unsigned long>(capture_rms_ppm / 1000000UL);
    unsigned long capture_rms_frac =
        static_cast<unsigned long>(capture_rms_ppm % 1000000UL);
    int written = snprintf(
        payload,
        sizeof(payload),
        "{\"schema_v\":%d,\"type\":\"%s\",\"seq\":%lu,\"input_mode\":\"%s\","
        "\"playback_state\":\"%s\",\"mic_muted\":%s,"
        "\"ptt\":%s,\"rms\":%lu.%06lu,\"client_ts_ms\":%lu}",
        kWireSchemaVersion,
        kClientAudioStateType,
        static_cast<unsigned long>(seq),
        ptt_mode_ ? kInputModePtt : kInputModeAuto,
        playback_active ? kPlaybackStateAgentSpeaking : kPlaybackStateIdle,
        mic_muted ? "true" : "false",
        ptt_held ? "true" : "false",
        capture_rms_whole,
        capture_rms_frac,
        static_cast<unsigned long>(client_ts_ms));
    if (written <= 0 || written >= static_cast<int>(sizeof(payload))) {
        return;
    }
    esp_err_t err = session_.PublishData(kClientAudioStateTopic, payload, state_changed);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Publish client audio state skipped: %s", esp_err_to_name(err));
        return;
    }
    if (state_changed || ptt_held || playback_active) {
        ESP_LOGI(TAG,
                 "Audio state seq=%lu mode=%s playback=%s mic_muted=%d ptt=%d "
                 "capture_rms=%lu.%03lu playback_rms=%lu.%03lu",
                 static_cast<unsigned long>(seq),
                 ptt_mode_ ? kInputModePtt : kInputModeAuto,
                 playback_active ? kPlaybackStateAgentSpeaking : kPlaybackStateIdle,
                 mic_muted ? 1 : 0,
                 ptt_held ? 1 : 0,
                 static_cast<unsigned long>(capture_rms_ppm / 1000000UL),
                 static_cast<unsigned long>((capture_rms_ppm % 1000000UL) / 1000UL),
                 static_cast<unsigned long>(playback_rms_ppm / 1000000UL),
                 static_cast<unsigned long>((playback_rms_ppm % 1000000UL) / 1000UL));
    }
    audio_state_sent_ = true;
    last_audio_playback_active_ = playback_active;
    last_audio_mic_muted_ = mic_muted;
    last_audio_ptt_ = ptt_held;
    last_audio_publish_us_ = now_us;
}

bool EidolonVoiceController::PlaybackActiveRecently() const
{
    int64_t last_playback_us = eidolon_livekit_board_last_playback_us();
    int64_t now_us = esp_timer_get_time();
    return last_playback_us > 0 && (now_us - last_playback_us) <= kPlaybackActiveWindowUs;
}

bool EidolonVoiceController::AgentOutputActiveRecently() const
{
    if (agent_phase_ == AgentPhase::AgentSpeaking) {
        return true;
    }
    return PlaybackActiveRecently();
}

void EidolonVoiceController::UpdateLocalPlaybackPhase(bool playback_active)
{
    if (!ptt_mode_ || standby_ || state_ != VoiceSessionState::InRoom) {
        return;
    }
    if (local_playback_ui_active_ == playback_active) {
        return;
    }
    local_playback_ui_active_ = playback_active;

    if (playback_active) {
        if (on_agent_phase_) {
            on_agent_phase_(AgentPhase::AgentSpeaking);
        }
    } else if (agent_phase_ != AgentPhase::AgentThinking &&
               agent_phase_ != AgentPhase::AgentSpeaking) {
        if (on_agent_phase_) {
            on_agent_phase_(AgentPhase::Silent);
        }
    }
    UpdateIdleAutoLeave();
}

esp_err_t EidolonVoiceController::StopLocalPlayback(const char* reason)
{
    esp_err_t flush_err = eidolon_livekit_board_flush_playback();
    if (flush_err != ESP_OK) {
        return flush_err;
    }
    ESP_LOGI(TAG, "Local playback stopped reason=%s", reason ? reason : "unspecified");
    UpdateLocalPlaybackPhase(false);
    DoAgentPhase(AgentPhase::Silent);
    return ESP_OK;
}

// ============================ Mic / PTT / agent phase ============================

void EidolonVoiceController::DoSetMicEnabled(bool enabled)
{
    mic_enabled_ = enabled;
    if (session_.IsConnected() && !standby_) {
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
        eidolon_livekit_board_set_capture_enabled(enabled);
    }
    ESP_LOGI(TAG, "Mic enabled=%d", enabled ? 1 : 0);
}

void EidolonVoiceController::DoPttPressed()
{
    if (!ptt_mode_) {
        return;
    }
    bool resumed_from_tail = ptt_release_tail_pending_;
    bool playback_recent = PlaybackActiveRecently();
    ESP_LOGI(TAG,
             "[ptt] press executing state=%s room_kind=%s gen=%lu connected=%d "
             "standby=%d ptt_active=%d tail=%d playback_recent=%d local_playback=%d "
             "agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             standby_ ? 1 : 0, ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, playback_recent ? 1 : 0,
             local_playback_ui_active_ ? 1 : 0, AgentPhaseName(agent_phase_));
    CancelPttReleaseTail();
    ptt_active_ = true;
    UpdateIdleAutoLeave();  // active: cancel the idle countdown
    // Hold-to-talk only applies in-room. Entering the room is an explicit tap (the
    // talk button is a "connect" button until connected) — a press while not
    // in-room is ignored rather than silently joining and dropping the first words.
    if (state_ != VoiceSessionState::InRoom) {
        ptt_active_ = false;
        ESP_LOGI(TAG, "[ptt] press ignored: not in room (tap to connect first) state=%s",
                 VoiceStateName(state_));
        return;
    }
    ESP_LOGI(TAG, "%s", resumed_from_tail ? "[ptt] press: release tail cancelled, mic open"
                                          : "[ptt] press: mic open");
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::DoPttReleased()
{
    if (!ptt_mode_) {
        return;
    }
    ESP_LOGI(TAG,
             "[ptt] release executing state=%s room_kind=%s gen=%lu connected=%d "
             "standby=%d ptt_active=%d tail=%d playback_recent=%d local_playback=%d "
             "agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             standby_ ? 1 : 0, ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, PlaybackActiveRecently() ? 1 : 0,
             local_playback_ui_active_ ? 1 : 0, AgentPhaseName(agent_phase_));
    if (!ptt_active_ && !ptt_release_tail_pending_) {
        ESP_LOGI(TAG, "[ptt] release ignored: not active");
        return;
    }
    if (!session_.IsConnected() || standby_ || state_ != VoiceSessionState::InRoom) {
        FinalizePttRelease("not_in_voice_room");
        return;
    }
    if (kPttReleaseTailUs == 0) {
        FinalizePttRelease("no_tail");
        return;
    }
    if (ptt_release_tail_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::PttReleaseTailCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_ptt_tail";
        if (esp_timer_create(&args, &ptt_release_tail_timer_) != ESP_OK) {
            ptt_release_tail_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create PTT release tail timer");
            FinalizePttRelease("tail_timer_create_failed");
            return;
        }
    }
    ptt_release_tail_pending_ = true;
    esp_timer_stop(ptt_release_tail_timer_);
    if (esp_timer_start_once(ptt_release_tail_timer_, kPttReleaseTailUs) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start PTT release tail timer");
        FinalizePttRelease("tail_timer_start_failed");
        return;
    }
    ESP_LOGI(TAG, "[ptt] release: keeping mic open for tail=%lums",
             static_cast<unsigned long>(CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS));
}

void EidolonVoiceController::DoPttReleaseTail()
{
    if (!ptt_mode_ || !ptt_release_tail_pending_) {
        return;
    }
    FinalizePttRelease("tail_elapsed");
}

void EidolonVoiceController::CancelPttReleaseTail()
{
    if (ptt_release_tail_timer_ != nullptr) {
        esp_timer_stop(ptt_release_tail_timer_);
    }
    ptt_release_tail_pending_ = false;
}

void EidolonVoiceController::FinalizePttRelease(const char* reason)
{
    CancelPttReleaseTail();
    ptt_active_ = false;
    if (session_.IsConnected() && !standby_) {
        ESP_LOGI(TAG, "[ptt] release: mic closed, turn committed (%s)",
                 reason ? reason : "unknown");
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
        ESP_LOGI(TAG, "[ptt] release: mic closed without publish (%s)",
                 reason ? reason : "not_connected");
        eidolon_livekit_board_set_capture_enabled(false);
    }
    UpdateIdleAutoLeave();  // turn done: start the idle countdown (if agent silent)
}

void EidolonVoiceController::DoAgentPhase(AgentPhase phase)
{
    bool changed = phase != agent_phase_;
    agent_phase_ = phase;
    if (changed) {
        ESP_LOGI(TAG, "Agent phase -> %s", AgentPhaseName(phase));
        if (session_.IsConnected() && !standby_) {
            PublishClientAudioState(AgentOutputActiveRecently());
        }
        UpdateIdleAutoLeave();  // agent busy disarms; back to silent arms the timer
        ResetFullDuplexIdleFallback("agent_phase");
    }
    if (on_agent_phase_) {
        on_agent_phase_(phase);
    }
}

void EidolonVoiceController::DoSessionActivity()
{
    ResetFullDuplexIdleFallback("transcription");
}

}  // namespace eidolon
