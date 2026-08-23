#include "commissioning_runtime.h"

#include "commissioning_orchestrator_core.h"
#include "commissioning_transaction.h"
#include "authority_locator.h"
#include "device_identity.h"
#include "device_provisioning.h"
#include "device_provisioning_protocol.h"
#include "hub_onboarding_protocol.h"
#include "hub_pinned_http.h"
#include "hub_trust_store.h"
#include "owner_trust_commissioning_worker.h"

#include <ssid_manager.h>
#include <wifi_manager.h>

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#define TAG "CommissioningRuntime"

namespace eidolon {
namespace {

constexpr size_t kQueueDepth = 8;
constexpr size_t kRuntimeStackBytes = 16384;

struct RuntimeMessage {
    CommissioningEvent event;
    std::string ssid;
    std::string password;
    struct TrustActorRequest* trust_request = nullptr;
};

struct TrustActorRequest {
    TrustActorRequest(const uint8_t* bytes, size_t size, uint32_t value)
        : payload(reinterpret_cast<const char*>(bytes), size), generation(value),
          completed(xSemaphoreCreateBinary()) {}
    ~TrustActorRequest()
    {
        if (completed != nullptr) vSemaphoreDelete(completed);
    }
    std::atomic<uint32_t> references{2};
    std::string payload;
    std::string response;
    uint32_t generation = 0;
    bool staged = false;
    SemaphoreHandle_t completed = nullptr;
};

void Release(TrustActorRequest* request)
{
    if (request != nullptr &&
        request->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete request;
    }
}

struct RuntimeState {
    CommissioningOrchestratorCore core;
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<uint32_t> visible_generation{0};
    std::atomic<bool> advertising{false};
    std::atomic<bool> active{false};
    std::atomic<bool> awaiting_station_route{false};
    std::mutex evidence_mutex;
    device_foundation::v1::CommissioningStatusEvidence evidence;
    std::mutex observer_mutex;
    CommissioningRuntime::Observer observer;
    CommissioningRuntime::OperationalRuntimeQuiescer operational_quiescer;
    std::string session_id;
    std::string candidate_ssid;
    std::string candidate_password;
    std::string candidate_id;
    bool previous_station_mode = false;
    bool committed_station_route_ready = false;
};

RuntimeState& State()
{
    static RuntimeState state;
    return state;
}

std::string RandomToken(size_t length)
{
    static constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string token;
    token.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        token.push_back(alphabet[esp_random() % (sizeof(alphabet) - 1)]);
    }
    return token;
}

bool Enqueue(RuntimeMessage* message)
{
    RuntimeState& state = State();
    if (message == nullptr || state.queue == nullptr ||
        xQueueSend(state.queue, &message, 0) != pdTRUE) {
        delete message;
        return false;
    }
    return true;
}

bool Enqueue(CommissioningEventType type, uint32_t generation = 0,
             std::string candidate_id = {})
{
    auto* message = new (std::nothrow) RuntimeMessage;
    if (message == nullptr) return false;
    message->event.type = type;
    message->event.generation = generation;
    message->event.candidate_id = std::move(candidate_id);
    return Enqueue(message);
}

esp_err_t StageTrustOnActor(uint32_t generation, const uint8_t* payload,
                            size_t payload_size, std::string& response,
                            bool& staged)
{
    response.clear();
    staged = false;
    if (payload == nullptr || payload_size == 0) return ESP_ERR_INVALID_ARG;
    auto* request = new (std::nothrow)
        TrustActorRequest(payload, payload_size, generation);
    auto* message = new (std::nothrow) RuntimeMessage;
    if (request == nullptr || request->completed == nullptr || message == nullptr) {
        delete message;
        if (request != nullptr) {
            request->references.store(1, std::memory_order_release);
            Release(request);
        }
        return ESP_ERR_NO_MEM;
    }
    message->trust_request = request;
    if (!Enqueue(message)) {
        Release(request);  // actor reference
        Release(request);  // callback reference
        return ESP_ERR_TIMEOUT;
    }
    const BaseType_t completed =
        xSemaphoreTake(request->completed, pdMS_TO_TICKS(15000));
    if (completed == pdTRUE) {
        response = request->response;
        staged = request->staged;
    }
    Release(request);  // callback reference
    return completed == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char* RuntimeStateName(CommissioningRuntimeState state)
{
    switch (state) {
    case CommissioningRuntimeState::Idle: return "idle";
    case CommissioningRuntimeState::PreparingIdentity: return "preparing-identity";
    case CommissioningRuntimeState::QuiescingOperationalRuntime:
        return "quiescing-operational-runtime";
    case CommissioningRuntimeState::AcquiringRadio: return "acquiring-radio";
    case CommissioningRuntimeState::StartingTransport: return "starting-transport";
    case CommissioningRuntimeState::Advertising: return "advertising";
    case CommissioningRuntimeState::SessionActive: return "session-active";
    case CommissioningRuntimeState::ApplyingConfiguration: return "applying-configuration";
    case CommissioningRuntimeState::ReturningToPreviousMode: return "returning-to-station";
    case CommissioningRuntimeState::RestoringPreviousMode: return "restoring-previous-mode";
    }
    return "unknown";
}

void PublishEvidence(RuntimeState& state)
{
    {
        std::lock_guard<std::mutex> lock(state.evidence_mutex);
        auto& evidence = state.evidence;
        evidence.session_id = state.session_id;
        evidence.setup_generation = state.core.generation();
        ++evidence.state_revision;
        if (evidence.state_revision == 0) ++evidence.state_revision;
    }
    CommissioningRuntime::Observer observer;
    {
        std::lock_guard<std::mutex> lock(state.observer_mutex);
        observer = state.observer;
    }
    const CommissioningRuntimeSnapshot snapshot{
        state.core.state(),
        state.core.generation(),
        state.core.transaction_committed(),
        state.committed_station_route_ready,
        state.previous_station_mode,
    };
    ESP_LOGI(TAG,
             "Confirmed state=%s generation=%lu committed=%d station_route_ready=%d",
             RuntimeStateName(snapshot.state),
             static_cast<unsigned long>(snapshot.generation),
             snapshot.transaction_committed ? 1 : 0,
             snapshot.station_route_ready ? 1 : 0);
    if (observer) {
        observer(snapshot);
    }
}

bool ValidateStagedOwnerRoute(uint32_t generation)
{
    OwnerTrustBundle trust;
    if (!OwnerTrustStore().LoadStaged(generation, trust)) return false;
    device_foundation::v1::OwnerDomainDescriptor staged;
    std::string staged_canonical;
    if (!ParseOwnerDomainDescriptor(trust.owner_domain_descriptor_json,
                                    staged, staged_canonical) ||
        staged.owner_domain_id != trust.owner_domain_id ||
        VerifyOwnerDomainDescriptor(
            staged, staged_canonical, trust.owner_root_certificate_pem,
            trust.authority_signing_certificate_pem) != ESP_OK) {
        return false;
    }
    for (const auto& endpoint : staged.endpoints) {
        if (endpoint.authority !=
            device_foundation::v1::LogicalAuthority::Admission) continue;
        HubHttpResponse response;
        if (HubHttpRequest("GET", endpoint.uri + "/descriptor",
                           trust.owner_root_certificate_pem, "", response) != ESP_OK ||
            response.status != 200) continue;
        device_foundation::v1::OwnerDomainDescriptor observed;
        std::string canonical;
        if (ParseOwnerDomainDescriptor(response.body, observed, canonical) &&
            observed.owner_domain_id == staged.owner_domain_id &&
            observed.directory_revision >= staged.directory_revision &&
            VerifyOwnerDomainDescriptor(
                observed, canonical, trust.owner_root_certificate_pem,
                trust.authority_signing_certificate_pem) == ESP_OK) {
            return true;
        }
    }
    return false;
}

bool CommitTransaction(RuntimeState& state, uint32_t generation)
{
    return eidolon::CommitCommissioningTransaction(
               generation, state.candidate_ssid, state.candidate_password) ==
           CommissioningTransactionResult::Committed;
}

void Apply(RuntimeState& state, const CommissioningEvent& event);

void Execute(RuntimeState& state, const CommissioningAction& action)
{
    CommissioningEvent completion;
    completion.generation = action.generation;
    completion.candidate_id = action.candidate_id;
    switch (action.type) {
    case CommissioningActionType::EnsureIdentity:
        completion.type = DeviceIdentity::GetInstance().EnsureKeypair() == ESP_OK
                              ? CommissioningEventType::IdentityReady
                              : CommissioningEventType::IdentityFailed;
        Apply(state, completion);
        break;
    case CommissioningActionType::QuiesceOperationalRuntime: {
        CommissioningRuntime::OperationalRuntimeQuiescer quiescer;
        {
            std::lock_guard<std::mutex> lock(state.observer_mutex);
            quiescer = state.operational_quiescer;
        }
        if (!quiescer) {
            // Composition must explicitly install either a real port or a
            // NullOperationalRuntimePort. Absence is not release evidence.
            completion.type =
                CommissioningEventType::OperationalRuntimeQuiesceFailed;
            Apply(state, completion);
            break;
        }
        quiescer([generation = action.generation](bool quiesced) {
            Enqueue(quiesced
                        ? CommissioningEventType::OperationalRuntimeQuiesced
                        : CommissioningEventType::OperationalRuntimeQuiesceFailed,
                    generation);
        });
        break;
    }
    case CommissioningActionType::AcquireCommissioningRadioLease:
        // The previous mode is an intent, not a momentary link condition. A
        // Station that is temporarily disconnected still has configured
        // networks and must be restored after cancel/failure.
        state.previous_station_mode =
            !SsidManager::GetInstance().GetSsidList().empty();
        WifiManager::GetInstance().StopStation();
        completion.type = CommissioningEventType::RadioAcquired;
        Apply(state, completion);
        break;
    case CommissioningActionType::StartTransport: {
        state.session_id = RandomToken(16);
        DeviceProvisioningService::Events events;
        events.stage_trust = &StageTrustOnActor;
        events.transport_ready = [](uint32_t generation, const std::string& id) {
            auto* message = new (std::nothrow) RuntimeMessage;
            if (message == nullptr) return;
            message->event.type = CommissioningEventType::TransportReady;
            message->event.generation = generation;
            message->event.transport_ready = {id, true, true};
            Enqueue(message);
        };
        events.authenticated_session_started = [](uint32_t generation) {
            Enqueue(CommissioningEventType::AuthenticatedSessionStarted, generation);
        };
        events.network_candidate_received = [](uint32_t generation,
                                                const std::string& ssid,
                                                const std::string& password) {
            auto* message = new (std::nothrow) RuntimeMessage;
            if (message == nullptr) return;
            message->event.type = CommissioningEventType::NetworkCandidateReceived;
            message->event.generation = generation;
            message->event.candidate_id = RandomToken(12);
            message->ssid = ssid;
            message->password = password;
            Enqueue(message);
        };
        events.wifi_connected = [](uint32_t generation) {
            Enqueue(CommissioningEventType::WifiConnected, generation);
        };
        events.wifi_connection_failed = [](uint32_t generation) {
            Enqueue(CommissioningEventType::OwnerRouteValidationFailed,
                    generation);
        };
        events.window_expired = [](uint32_t generation) {
            Enqueue(CommissioningEventType::WindowExpired, generation);
        };
        events.transport_stopped = [](uint32_t generation) {
            Enqueue(CommissioningEventType::TransportStopped, generation);
        };
        events.transport_ended_unexpectedly = [](uint32_t generation) {
            Enqueue(CommissioningEventType::TransportEndedUnexpectedly,
                    generation);
        };
        events.commissioning_status = [](uint32_t generation) {
            RuntimeState& current = State();
            std::lock_guard<std::mutex> lock(current.evidence_mutex);
            if (generation != current.evidence.setup_generation) return std::string{};
            return BuildCommissioningStatusJson(current.evidence);
        };
        events.terminal_ack = [](uint32_t generation, const std::string& payload) {
            RuntimeState& current = State();
            device_foundation::v1::CommissioningTerminalAck ack;
            {
                std::lock_guard<std::mutex> lock(current.evidence_mutex);
                if (!ParseCommissioningTerminalAck(payload, ack) ||
                    generation != current.evidence.setup_generation ||
                    ack.session_id != current.evidence.session_id ||
                    ack.setup_generation != current.evidence.setup_generation ||
                    ack.observed_state_revision != current.evidence.state_revision ||
                    current.evidence.state !=
                        device_foundation::v1::CommissioningStatusState::Committed) {
                    return false;
                }
            }
            return Enqueue(CommissioningEventType::ControllerObservedTerminal,
                           generation);
        };
        const esp_err_t result = DeviceProvisioningService::GetInstance().Start(
            action.generation, state.session_id, std::move(events));
        if (result != ESP_OK) {
            completion.type = CommissioningEventType::TransportStartFailed;
            Apply(state, completion);
        }
        break;
    }
    case CommissioningActionType::StageNetworkCandidate:
        completion.type = CommissioningEventType::NetworkCandidateStaged;
        Apply(state, completion);
        break;
    case CommissioningActionType::ValidateOwnerRoute:
        completion.type = ValidateStagedOwnerRoute(action.generation)
                              ? CommissioningEventType::OwnerRouteValidated
                              : CommissioningEventType::OwnerRouteValidationFailed;
        Apply(state, completion);
        break;
    case CommissioningActionType::CommitCommissioningTransaction:
        completion.type = CommitTransaction(state, action.generation)
                              ? CommissioningEventType::CommissioningTransactionCommitted
                              : CommissioningEventType::CommissioningTransactionCommitFailed;
        Apply(state, completion);
        break;
    case CommissioningActionType::RollbackCommissioningTransaction:
        if (!RollbackPendingCommissioningTransaction(action.generation)) {
            ESP_LOGE(TAG, "Refusing unsafe rollback for generation %lu",
                     static_cast<unsigned long>(action.generation));
            break;
        }
        completion.type = CommissioningEventType::CommissioningTransactionRolledBack;
        Apply(state, completion);
        break;
    case CommissioningActionType::StopTransport:
        DeviceProvisioningService::GetInstance().Stop(action.generation);
        break;
    case CommissioningActionType::RestorePreviousRadioMode:
        // A committed act transitions to Station with the new candidate. An
        // aborted act restores Station only if it was the mode we leased away.
        // Initial uncommissioned setup therefore does not silently invent a
        // Station mode on cancel/timeout.
        if (state.core.transaction_committed()) {
            // Starting a mode is a command, not evidence. Keep the generation
            // alive until WifiBoard submits the fresh connected route emitted
            // by the restored Station implementation.
            state.awaiting_station_route.store(true, std::memory_order_release);
            WifiManager::GetInstance().StartStation();
            break;
        }
        if (state.previous_station_mode) {
            WifiManager::GetInstance().StartStation();
        }
        completion.type = CommissioningEventType::PreviousModeRestored;
        Apply(state, completion);
        break;
    case CommissioningActionType::PublishConfirmedState:
        PublishEvidence(state);
        break;
    }
}

void Apply(RuntimeState& state, const CommissioningEvent& event)
{
    if (event.type == CommissioningEventType::OpenRequested &&
        state.core.state() == CommissioningRuntimeState::Idle) {
        {
            std::lock_guard<std::mutex> lock(state.evidence_mutex);
            state.evidence = {};
        }
        state.session_id.clear();
        state.candidate_ssid.clear();
        state.candidate_password.clear();
        state.candidate_id.clear();
        state.previous_station_mode = false;
        state.committed_station_route_ready = false;
        state.awaiting_station_route.store(false, std::memory_order_release);
    }
    CommissioningEvent resolved = event;
    if ((resolved.type == CommissioningEventType::WifiConnected ||
         resolved.type == CommissioningEventType::OwnerRouteValidationFailed) &&
        resolved.candidate_id.empty()) {
        resolved.candidate_id = state.candidate_id;
    }
    {
        std::lock_guard<std::mutex> lock(state.evidence_mutex);
        using Status = device_foundation::v1::CommissioningStatusState;
        switch (resolved.type) {
        case CommissioningEventType::WifiConnected:
            state.evidence.conditions.wifi_connected = true;
            break;
        case CommissioningEventType::OwnerRouteValidated:
            state.evidence.conditions.owner_route_validated = true;
            break;
        case CommissioningEventType::CommissioningTransactionCommitted:
            state.evidence.conditions.trust_committed = true;
            state.evidence.conditions.network_committed = true;
            state.evidence.state = Status::Committed;
            break;
        case CommissioningEventType::CommissioningTransactionRolledBack:
            state.evidence = {};
            state.evidence.state = Status::RolledBack;
            state.evidence.failure_code =
                device_foundation::v1::CommissioningFailureCode::Internal;
            break;
        default:
            break;
        }
    }
    const bool completes_committed_route =
        resolved.type == CommissioningEventType::StationRouteReady &&
        state.core.transaction_committed();
    const auto actions = state.core.Handle(resolved);
    if (completes_committed_route &&
        state.core.state() == CommissioningRuntimeState::Idle) {
        state.committed_station_route_ready = true;
        state.awaiting_station_route.store(false, std::memory_order_release);
    }
    state.visible_generation.store(state.core.generation(), std::memory_order_release);
    state.advertising.store(
        state.core.state() == CommissioningRuntimeState::Advertising,
        std::memory_order_release);
    state.active.store(state.core.state() != CommissioningRuntimeState::Idle,
                       std::memory_order_release);
    for (const auto& action : actions) Execute(state, action);
}

void Actor(void*)
{
    RuntimeState& state = State();
    while (true) {
        RuntimeMessage* raw = nullptr;
        if (xQueueReceive(state.queue, &raw, portMAX_DELAY) != pdTRUE || raw == nullptr) {
            continue;
        }
        std::unique_ptr<RuntimeMessage> message(raw);
        if (message->trust_request != nullptr) {
            TrustActorRequest* request = message->trust_request;
            const esp_err_t result =
                OwnerTrustCommissioningWorker::GetInstance().Submit(
                    request->generation,
                    reinterpret_cast<const uint8_t*>(request->payload.data()),
                    request->payload.size(), request->response, request->staged);
            CommissioningEvent trust_event;
            trust_event.generation = request->generation;
            trust_event.type = result == ESP_OK && request->staged
                                   ? CommissioningEventType::TrustStaged
                                   : CommissioningEventType::TrustStageFailed;
            Apply(state, trust_event);
            xSemaphoreGive(request->completed);
            Release(request);  // actor reference
            continue;
        }
        if (message->event.type == CommissioningEventType::NetworkCandidateReceived) {
            state.candidate_ssid = std::move(message->ssid);
            state.candidate_password = std::move(message->password);
            state.candidate_id = message->event.candidate_id;
        }
        Apply(state, message->event);
    }
}

bool EnsureActor()
{
    RuntimeState& state = State();
    if (state.queue == nullptr) {
        state.queue = xQueueCreate(kQueueDepth, sizeof(RuntimeMessage*));
        if (state.queue == nullptr) return false;
    }
    if (state.task == nullptr &&
        xTaskCreate(&Actor, "commissioning", kRuntimeStackBytes, nullptr, 5,
                    &state.task) != pdPASS) {
        vQueueDelete(state.queue);
        state.queue = nullptr;
        return false;
    }
    return true;
}

}  // namespace

CommissioningRuntime& CommissioningRuntime::GetInstance()
{
    static CommissioningRuntime runtime;
    return runtime;
}

bool CommissioningRuntime::RequestOpen()
{
    return EnsureActor() && Enqueue(CommissioningEventType::OpenRequested);
}

bool CommissioningRuntime::RequestCancel()
{
    RuntimeState& state = State();
    const uint32_t generation =
        state.visible_generation.load(std::memory_order_acquire);
    return generation != 0 &&
           Enqueue(CommissioningEventType::CancelRequested, generation);
}

bool CommissioningRuntime::NotifyStationRouteReady()
{
    RuntimeState& state = State();
    if (!state.awaiting_station_route.exchange(false,
                                                std::memory_order_acq_rel)) {
        return false;
    }
    const uint32_t generation =
        state.visible_generation.load(std::memory_order_acquire);
    if (generation == 0 ||
        !Enqueue(CommissioningEventType::StationRouteReady, generation)) {
        state.awaiting_station_route.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void CommissioningRuntime::SetObserver(Observer observer)
{
    RuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.observer_mutex);
    state.observer = std::move(observer);
}

void CommissioningRuntime::SetOperationalRuntimeQuiescer(
    OperationalRuntimeQuiescer quiescer)
{
    RuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.observer_mutex);
    state.operational_quiescer = std::move(quiescer);
}

bool CommissioningRuntime::IsAdvertising() const
{
    return State().advertising.load(std::memory_order_acquire);
}

bool CommissioningRuntime::IsInProgress() const
{
    return State().active.load(std::memory_order_acquire);
}

}  // namespace eidolon
