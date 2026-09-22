#include "commissioning_runtime.h"

#include "commissioning_orchestrator_core.h"
#include "commissioning_transaction.h"
#include "authority_locator.h"
#include "device_identity.h"
#include "esp_idf_commissioning_credential_store.h"
#include <esp_system.h>
#include "device_provisioning.h"
#include "device_provisioning_protocol.h"
#include "hub_onboarding_protocol.h"
#include "hub_pinned_http.h"
#include "hub_trust_store.h"
#include "owner_trust_commissioning_worker.h"
#include "provisioning_window_policy_core.h"

#include "sdkconfig.h"

#include <ssid_manager.h>
#include <wifi_manager.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
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
    // Guards the queue/task handles and the retirement decision together. The
    // actor retires itself from inside its own loop while other tasks — the
    // BOOT gesture, the no-profile auto-open, Wi-Fi and provisioning callbacks
    // — may be posting to that same queue. Deciding to leave and accepting new
    // work have to be one atomic choice, or a message lands in a queue that is
    // being deleted.
    std::mutex lifecycle;
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<uint32_t> visible_generation{0};
    std::atomic<bool> advertising{false};
    std::atomic<bool> active{false};
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
    bool identity_replaced = false;
    bool station_restore_requested = false;
    std::string failure_hint;
    int64_t terminal_deadline_us = 0;
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

// Caller holds state.lifecycle. A null queue means the actor has retired and
// nobody has asked for a new window yet; the message is refused rather than
// posted into a handle that no longer exists.
bool EnqueueLocked(RuntimeState& state, RuntimeMessage* message)
{
    if (message == nullptr || state.queue == nullptr ||
        xQueueSend(state.queue, &message, 0) != pdTRUE) {
        delete message;
        return false;
    }
    return true;
}

bool Enqueue(RuntimeMessage* message)
{
    RuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.lifecycle);
    return EnqueueLocked(state, message);
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
    case CommissioningRuntimeState::RecoveringConfiguration: return "recovering-configuration";
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
    uint32_t revision;
    {
        std::lock_guard<std::mutex> lock(state.evidence_mutex);
        auto& evidence = state.evidence;
        evidence.session_id = state.session_id;
        evidence.setup_generation = state.core.generation();
        ++evidence.state_revision;
        if (evidence.state_revision == 0) ++evidence.state_revision;
        revision = evidence.state_revision;
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
        state.station_restore_requested,
        revision,
        state.failure_hint,
    };
    ESP_LOGI(TAG,
             "Confirmed state=%s generation=%lu committed=%d revision=%lu",
             RuntimeStateName(snapshot.state),
             static_cast<unsigned long>(snapshot.generation),
             snapshot.transaction_committed ? 1 : 0,
             static_cast<unsigned long>(snapshot.revision));
    if (observer) {
        observer(snapshot);
    }
}

// Re-fetch the staged Owner document over the joined network and confirm the
// Owner route serves the same directory the setup channel handed over.
//
// Every failure here is logged and each message is distinct. This step used to
// return false six ways with no log at all: the Owner saw only "configuration
// failed", and neither device, app nor Host could name the missing fact. That
// silence is what let a wrong descriptor route survive — the request 404'd on
// every Host and the transaction rolled back with nothing written down.
device_foundation::v1::CommissioningFailureCode ValidateStagedOwnerRoute(uint32_t generation)
{
    using Failure = device_foundation::v1::CommissioningFailureCode;
    OwnerTrustBundle trust;
    if (!OwnerTrustStore().LoadStaged(generation, trust)) {
        ESP_LOGE(TAG,
                 "Owner route rejected: staged Owner trust for generation %lu "
                 "is unreadable",
                 static_cast<unsigned long>(generation));
        return Failure::StorageUnavailable;
    }
    device_foundation::v1::OwnerDomainDescriptor staged;
    std::string staged_canonical;
    if (!ParseOwnerDomainDescriptor(trust.owner_domain_descriptor_json, staged,
                                    staged_canonical)) {
        ESP_LOGE(TAG,
                 "Owner route rejected: staged Owner Domain descriptor does not "
                 "parse against the canonical contract");
        return Failure::OwnerIdentityMismatch;
    }
    if (staged.owner_domain_id != trust.owner_domain_id) {
        ESP_LOGE(TAG,
                 "Owner route rejected: staged descriptor names Owner Domain "
                 "'%s' but the staged trust names '%s'",
                 staged.owner_domain_id.c_str(), trust.owner_domain_id.c_str());
        return Failure::OwnerIdentityMismatch;
    }
    if (VerifyOwnerDomainDescriptor(
            staged, staged_canonical, trust.owner_root_certificate_pem,
            trust.authority_signing_certificate_pem) != ESP_OK) {
        ESP_LOGE(TAG,
                 "Owner route rejected: staged descriptor signature does not "
                 "verify under the staged Owner root");
        return Failure::OwnerIdentityMismatch;
    }
    // The parser already requires an absolute https route, so this restates the
    // invariant where the request is actually built: a URL handed to the HTTP
    // client must never come from an unchecked field of a signed document.
    if (staged.descriptor_uri.rfind("https://", 0) != 0) {
        ESP_LOGE(TAG,
                 "Owner route rejected: staged descriptor publishes no absolute "
                 "https descriptor_uri");
        return Failure::OwnerIdentityMismatch;
    }
    HubHttpResponse response;
    const esp_err_t transport =
        HubHttpRequest("GET", staged.descriptor_uri,
                       trust.owner_root_certificate_pem, "", response);
    if (transport != ESP_OK || response.status != 200) {
        ESP_LOGE(TAG,
                 "Owner route rejected: GET %s failed (transport=%s status=%d)",
                 staged.descriptor_uri.c_str(), esp_err_to_name(transport),
                 response.status);
        return Failure::OwnerRouteUnavailable;
    }
    device_foundation::v1::OwnerDomainDescriptor observed;
    std::string canonical;
    if (!ParseOwnerDomainDescriptor(response.body, observed, canonical) ||
        observed.owner_domain_id != staged.owner_domain_id ||
        observed.directory_revision < staged.directory_revision ||
        VerifyOwnerDomainDescriptor(
            observed, canonical, trust.owner_root_certificate_pem,
            trust.authority_signing_certificate_pem) != ESP_OK) {
        ESP_LOGE(TAG,
                 "Owner route rejected: %s served a directory that is not the "
                 "staged one (owner='%s' revision=%lu staged revision=%lu)",
                 staged.descriptor_uri.c_str(), observed.owner_domain_id.c_str(),
                 static_cast<unsigned long>(observed.directory_revision),
                 static_cast<unsigned long>(staged.directory_revision));
        return Failure::OwnerIdentityMismatch;
    }
    return Failure::None;
}


void Apply(RuntimeState& state, const CommissioningEvent& event);

void Execute(RuntimeState& state, const CommissioningAction& action)
{
    CommissioningEvent completion;
    completion.generation = action.generation;
    completion.candidate_id = action.candidate_id;
    switch (action.type) {
    case CommissioningActionType::EnsureIdentity: {
        // Two independent reasons a setup window cannot open, collapsed into
        // one bool and then into one unexplained return to Idle. From outside
        // that is a button that does nothing: the actor announces
        // preparing-identity, retires ten milliseconds later, and never says
        // which of the two stopped it.
        // A physical retry resumes an existing decision before opening a new
        // generation; no new candidate may overwrite the unresolved snapshot.
        bool recovered_replacement = false;
        const bool recovered = RecoverPendingCommissioningTransaction(&recovered_replacement);
        if (recovered && recovered_replacement) {
            // A boot-blocked Owner replacement can also finish on physical
            // retry. Retire the previous Owner's caches before opening setup.
            esp_restart();
            break;
        }
        const bool transaction_allows = recovered || CommissioningTransactionNeedsFreshIdentity();
        const bool ready =
            transaction_allows && DeviceIdentity::GetInstance().EnsureKeypair() == ESP_OK;
        if (!ready) {
            state.failure_hint = transaction_allows
                ? "Device identity needs recovery"
                : "Configuration recovery blocked. Hold BOOT to retry setup";
            ESP_LOGE(TAG, "cannot prepare an identity: %s",
                     transaction_allows
                         ? "no usable operational key"
                         : "an unfinished commissioning transaction still owns this device");
        }
        completion.type = ready ? CommissioningEventType::IdentityReady
                                : CommissioningEventType::IdentityFailed;
        Apply(state, completion);
        break;
    }
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
            Enqueue(CommissioningEventType::WifiConnectionFailed,
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
                    (current.evidence.state !=
                        device_foundation::v1::CommissioningStatusState::Committed &&
                     current.evidence.state !=
                        device_foundation::v1::CommissioningStatusState::RolledBack)) {
                    return false;
                }
            }
            return true;
        };
        events.terminal_ack_response_finished = [](uint32_t generation) {
            Enqueue(CommissioningEventType::ControllerObservedTerminal, generation);
        };
        // The trust store, not the caller of RequestOpen, decides whether this
        // act is a first claim or an Owner reopening setup on a device that is
        // already theirs: both intents arrive through the same gesture, and
        // only the store knows whether there is an Owner Domain to protect.
        // Read once per generation so the answer cannot change underneath the
        // window that was armed from it.
        const ProvisioningWindowPolicy window = DecideProvisioningWindow(
            ProvisioningWindowTriggerFor(
                OwnerTrustStore().CommissionedOwnerDomainId()));
        const esp_err_t result = DeviceProvisioningService::GetInstance().Start(
            action.generation, state.session_id, window, std::move(events));
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
    case CommissioningActionType::ValidateOwnerRoute: {
        const auto failure = ValidateStagedOwnerRoute(action.generation);
        {
            std::lock_guard<std::mutex> lock(state.evidence_mutex);
            state.evidence.failure_code = failure;
        }
        completion.type = failure == device_foundation::v1::CommissioningFailureCode::None
                              ? CommissioningEventType::OwnerRouteValidated
                              : CommissioningEventType::OwnerRouteValidationFailed;
        Apply(state, completion);
        break;
    }
    case CommissioningActionType::CommitCommissioningTransaction: {
        const auto result = CommitCommissioningTransaction(
            action.generation, state.candidate_ssid, state.candidate_password,
            &state.identity_replaced);
        completion.type = result == CommissioningTransactionResult::Committed
            ? CommissioningEventType::CommissioningTransactionCommitted
            : result == CommissioningTransactionResult::RecoveryRequired
                ? CommissioningEventType::CommissioningTransactionRecoveryRequired
                : CommissioningEventType::CommissioningTransactionCommitFailed;
        Apply(state, completion);
        break;
    }
    case CommissioningActionType::RecoverCommissioningTransaction:
        if (RecoverPendingCommissioningTransaction(&state.identity_replaced)) {
            completion.type = CommissioningEventType::CommissioningTransactionCommitted;
            Apply(state, completion);
        }
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
        if (!state.core.transaction_committed() &&
            !RollbackPendingCommissioningTransaction(action.generation)) {
            ESP_LOGE(TAG, "Could not discard uncommitted candidate");
        }
        DeviceProvisioningService::GetInstance().Stop(action.generation);
        break;
    case CommissioningActionType::RestorePreviousRadioMode:
        if (state.core.transaction_committed() &&
            state.identity_replaced) {
            // Storage is coherent and the setup transport is closed. A restart
            // retires every old Owner's in-memory session, binding and cache.
            esp_restart();
            break;
        }
        // A committed act transitions to Station with the new candidate. An
        // aborted act restores Station only if it was the mode we leased away.
        // Initial uncommissioned setup therefore does not silently invent a
        // Station mode on cancel/timeout.
        state.previous_station_mode =
            state.core.transaction_committed() || state.previous_station_mode;
        if (state.previous_station_mode) {
            WifiManager::GetInstance().StartStation();
            state.station_restore_requested = true;
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
        state.identity_replaced = false;
        state.station_restore_requested = false;
        state.failure_hint.clear();
        state.terminal_deadline_us = 0;
        state.session_id.clear();
        state.candidate_ssid.clear();
        state.candidate_password.clear();
        state.candidate_id.clear();
        state.previous_station_mode =
            !SsidManager::GetInstance().GetSsidList().empty();
    }
    if (event.type != CommissioningEventType::OpenRequested &&
        (event.generation != state.core.generation() ||
         state.core.state() == CommissioningRuntimeState::Idle)) return;
    if (!event.candidate_id.empty() && !state.candidate_id.empty() &&
        event.candidate_id != state.candidate_id) return;
    // Late radio/validation callbacks must not rewrite a terminal snapshot.
    switch (event.type) {
    case CommissioningEventType::WifiConnected:
    case CommissioningEventType::WifiConnectionFailed:
    case CommissioningEventType::OwnerRouteValidated:
    case CommissioningEventType::OwnerRouteValidationFailed:
    case CommissioningEventType::CommissioningTransactionCommitFailed:
        if (state.core.state() != CommissioningRuntimeState::ApplyingConfiguration ||
            state.core.transaction_committed()) return;
        break;
    default:
        break;
    }
    CommissioningEvent resolved = event;
    if ((resolved.type == CommissioningEventType::WifiConnected ||
         resolved.type == CommissioningEventType::OwnerRouteValidationFailed ||
         resolved.type == CommissioningEventType::WifiConnectionFailed) &&
        resolved.candidate_id.empty()) {
        resolved.candidate_id = state.candidate_id;
    }
    {
        std::lock_guard<std::mutex> lock(state.evidence_mutex);
        using Status = device_foundation::v1::CommissioningStatusState;
        using Failure = device_foundation::v1::CommissioningFailureCode;
        switch (resolved.type) {
        case CommissioningEventType::WifiConnectionFailed:
            state.evidence.failure_code = Failure::NetworkRejected;
            state.failure_hint = "Setup failed: Wi-Fi rejected. Previous setup retained";
            break;
        case CommissioningEventType::OwnerRouteValidationFailed:
            state.failure_hint = "Setup failed: selected Host validation failed. Previous setup retained";
            break;
        case CommissioningEventType::CommissioningTransactionCommitFailed:
            state.evidence.failure_code = Failure::StorageUnavailable;
            state.failure_hint = "Setup failed: storage unavailable. Previous setup retained";
            break;
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
            state.terminal_deadline_us = esp_timer_get_time() + 30 * 1000000LL;
            break;
        case CommissioningEventType::CommissioningTransactionRolledBack: {
            // UI/status acknowledgments share this revision. Rollback clears
            // candidate conditions, but must not reuse an earlier revision in
            // the same generation and accidentally accept its queued snapshot.
            const auto revision = state.evidence.state_revision;
            const auto failure = state.evidence.failure_code;
            state.evidence = {};
            state.evidence.state_revision = revision;
            state.evidence.state = Status::RolledBack;
            state.evidence.failure_code = failure == Failure::None ? Failure::Cancelled : failure;
            state.terminal_deadline_us = esp_timer_get_time() + 30 * 1000000LL;
            break;
        }
        default:
            break;
        }
    }
    const auto actions = state.core.Handle(resolved);
    state.visible_generation.store(state.core.generation(), std::memory_order_release);
    state.advertising.store(
        state.core.state() == CommissioningRuntimeState::Advertising,
        std::memory_order_release);
    state.active.store(state.core.state() != CommissioningRuntimeState::Idle,
                       std::memory_order_release);
    for (const auto& action : actions) Execute(state, action);
}

// True once this generation is over and nothing is waiting to start another.
// Idle is reached by exactly three routes — a failure before any transport or
// radio lease existed, a restore that finished, and a committed act whose
// radio mode was handed back — and every one of them has already run the
// transport's single cleanup path. So Idle is not merely "not busy": it is the
// point at which this actor owns nothing.
//
// The queue check is not belt-and-braces. An erased device re-opens its own
// window with nobody touching the BOOT key, so "returned to Idle" and "nobody
// wants commissioning" are genuinely different questions, and only the second
// one permits leaving.
bool MayRetire(RuntimeState& state)
{
    return state.core.state() == CommissioningRuntimeState::Idle &&
           uxQueueMessagesWaiting(state.queue) == 0;
}

// Returns true once the actor has released its queue and may delete itself.
bool RetireIfIdle(RuntimeState& state)
{
    if (!MayRetire(state)) return false;
    std::lock_guard<std::mutex> lock(state.lifecycle);
    // Re-decide under the lock. RequestOpen() takes it across BOTH ensuring the
    // actor exists and posting OpenRequested, so a window asked for since the
    // check above is already visible here as a queued message.
    if (!MayRetire(state)) return false;
    QueueHandle_t queue = state.queue;
    state.queue = nullptr;
    state.task = nullptr;
    vQueueDelete(queue);
    return true;
}

void Actor(void*)
{
    RuntimeState& state = State();
    while (true) {
        if (state.terminal_deadline_us != 0 &&
            esp_timer_get_time() >= state.terminal_deadline_us) {
            state.terminal_deadline_us = 0;
            CommissioningEvent expired;
            expired.type = CommissioningEventType::TerminalDeliveryExpired;
            expired.generation = state.core.generation();
            Apply(state, expired);
            if (RetireIfIdle(state)) break;
        }
        RuntimeMessage* raw = nullptr;
        const TickType_t wait = state.terminal_deadline_us == 0
            ? portMAX_DELAY : pdMS_TO_TICKS(250);
        if (xQueueReceive(state.queue, &raw, wait) != pdTRUE || raw == nullptr) continue;
        std::unique_ptr<RuntimeMessage> message(raw);
        if (message->trust_request != nullptr) {
            TrustActorRequest* request = message->trust_request;
            const bool accepting = request->generation == state.core.generation() &&
                state.core.state() == CommissioningRuntimeState::SessionActive;
            const esp_err_t result = accepting
                ? OwnerTrustCommissioningWorker::GetInstance().Submit(
                    request->generation,
                    reinterpret_cast<const uint8_t*>(request->payload.data()),
                    request->payload.size(), request->response, request->staged)
                : ESP_ERR_INVALID_STATE;
            CommissioningEvent trust_event;
            trust_event.generation = request->generation;
            trust_event.type = result == ESP_OK && request->staged
                                   ? CommissioningEventType::TrustStaged
                                   : CommissioningEventType::TrustStageFailed;
            Apply(state, trust_event);
            xSemaphoreGive(request->completed);
            Release(request);  // actor reference
            if (RetireIfIdle(state)) break;
            continue;
        }
        if (message->event.type == CommissioningEventType::NetworkCandidateReceived &&
            message->event.generation == state.core.generation() &&
            state.core.state() == CommissioningRuntimeState::SessionActive) {
            state.candidate_ssid = std::move(message->ssid);
            state.candidate_password = std::move(message->password);
            state.candidate_id = message->event.candidate_id;
        }
        Apply(state, message->event);
        // The safe point: the event is applied and every action it produced has
        // run, so the actor holds nothing but its own stack.
        if (RetireIfIdle(state)) break;
    }
    ESP_LOGI(TAG,
             "Commissioning actor retired; %u bytes of internal RAM returned to "
             "the heap the LiveKit engine has to build itself out of",
             static_cast<unsigned>(kRuntimeStackBytes));
    vTaskDelete(nullptr);
}

// Caller holds state.lifecycle.
bool EnsureActorLocked(RuntimeState& state)
{
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
    RuntimeState& state = State();
    // One lock across both halves. Splitting them would let the actor decide to
    // retire between the task existing and the request reaching its queue, and
    // the gesture would be swallowed by a queue on its way to being deleted.
    std::lock_guard<std::mutex> lock(state.lifecycle);
    // Build the request BEFORE the actor exists. An actor created for a request
    // that then fails to arrive would block on an empty queue forever, and
    // because retirement is only ever decided after receiving a message, it
    // could never leave — 16 KiB stranded by the very failure path meant to
    // report that memory was short.
    auto* message = new (std::nothrow) RuntimeMessage;
    if (message == nullptr) return false;
    message->event.type = CommissioningEventType::OpenRequested;
    message->event.generation = 0;
    if (!EnsureActorLocked(state)) {
        delete message;
        return false;
    }
    return EnqueueLocked(state, message);
}

bool CommissioningRuntime::RequestCancel()
{
    RuntimeState& state = State();
    const uint32_t generation =
        state.visible_generation.load(std::memory_order_acquire);
    return generation != 0 &&
           Enqueue(CommissioningEventType::CancelRequested, generation);
}

bool CommissioningRuntime::IsCurrent(
    const CommissioningRuntimeSnapshot& snapshot) const
{
    RuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.evidence_mutex);
    return snapshot.generation == state.evidence.setup_generation &&
           snapshot.revision == state.evidence.state_revision &&
           snapshot.generation == Generation() &&
           (snapshot.state != CommissioningRuntimeState::Idle) == IsInProgress();
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

uint32_t CommissioningRuntime::Generation() const {
    return State().visible_generation.load(std::memory_order_acquire);
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
