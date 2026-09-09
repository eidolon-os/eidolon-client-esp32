#include "owner_trust_commissioning_worker.h"

#include "authority_locator.h"
#include "device_provisioning_protocol.h"
#include "device_identity.h"
#include "hub_trust_store.h"
#include "esp_idf_commissioning_credential_store.h"
#include "owner_trust_commissioner.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cJSON.h>
#include <mutex>
#include <new>
#include <utility>

#define TAG "OwnerTrustWorker"

namespace eidolon {
namespace {

constexpr size_t kQueueDepth = 1;
constexpr int64_t kWorkDeadlineUs =
    static_cast<int64_t>(CONFIG_EIDOLON_OWNER_TRUST_WORK_DEADLINE_MS) * 1000;
constexpr TickType_t kResponseWait = pdMS_TO_TICKS(
    CONFIG_EIDOLON_OWNER_TRUST_WORK_DEADLINE_MS + 2000);

class Esp32OwnerTrustVerifier final : public OwnerTrustVerifierPort {
public:
    bool Verify(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& canonical_signing_bytes,
        const std::string& owner_root_certificate_pem,
        const std::string& authority_signing_certificate_pem) override
    {
        return VerifyOwnerDomainDescriptor(
                   descriptor,
                   canonical_signing_bytes,
                   owner_root_certificate_pem,
                   authority_signing_certificate_pem) == ESP_OK;
    }
};

struct Request {
    explicit Request(std::string input, uint32_t generation)
        : payload(std::move(input)), transport_generation(generation),
          deadline_us(esp_timer_get_time() + kWorkDeadlineUs),
          completed(xSemaphoreCreateBinary()) {}

    ~Request()
    {
        if (completed != nullptr) vSemaphoreDelete(completed);
    }

    std::atomic<uint32_t> references{1};
    std::string payload;
    std::string response;
    bool trust_staged = false;
    uint32_t transport_generation = 0;
    int64_t deadline_us = 0;
    SemaphoreHandle_t completed = nullptr;
};

void Retain(Request* request)
{
    request->references.fetch_add(1, std::memory_order_relaxed);
}

void Release(Request* request)
{
    if (request->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete request;
    }
}

struct Runtime {
    Esp32OwnerTrustVerifier verifier;
    OwnerTrustStore store;
    EspIdfCommissioningCredentialStore& credentials =
        EspIdfCommissioningCredentialStore::GetInstance();
    OwnerTrustCommissioner commissioner{verifier, store, credentials};
    // Guards queue/task and the retirement decision. The worker is the only
    // task that clears these, and it does so while another task may be asking
    // it to; without the lock that is a data race on two raw handles.
    std::mutex lifecycle;
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<uint32_t> active_generation{0};
    // Retirement is ASKED FOR, never done to the worker. vTaskDelete() from
    // outside would be simpler and is wrong: this task spends its time inside
    // X.509/ES256 verification and NVS commits, so an external delete can land
    // mid-write and leave the trust store — the hardest thing on this board to
    // recover — half written, holding an NVS lock nothing will ever release.
    std::atomic<bool> retire_requested{false};
    SemaphoreHandle_t retired = nullptr;
};

Runtime& State()
{
    static Runtime runtime;
    return runtime;
}

const char* RefusalReason(OwnerTrustCommissioningCode code)
{
    switch (code) {
    case OwnerTrustCommissioningCode::Unsupported:
        return "handover is not supported";
    case OwnerTrustCommissioningCode::Invalid:
        return "owner trust is invalid";
    case OwnerTrustCommissioningCode::Stale:
        return "setup session is no longer active";
    case OwnerTrustCommissioningCode::StorageUnavailable:
        return "owner trust was not stored";
    case OwnerTrustCommissioningCode::Prepared:
    case OwnerTrustCommissioningCode::Staged:
        break;
    }
    return "owner trust was refused";
}

// Hand back anything still queued instead of deleting the queue underneath it.
// Each straggler has a Submit() caller blocked on its semaphore, and a caller
// left to wait out its full timeout for an answer that can never arrive is a
// worse failure than the refusal it is owed. Releases the worker's reference,
// so the request is freed by whichever side lets go last.
void ReleaseQueuedRequests(Runtime& runtime)
{
    Request* straggler = nullptr;
    while (xQueueReceive(runtime.queue, &straggler, 0) == pdTRUE) {
        if (straggler == nullptr) continue;
        straggler->response =
            BuildTrustRefusedJson("setup session is no longer active");
        xSemaphoreGive(straggler->completed);
        Release(straggler);  // worker reference
    }
}

// One trust decision, start to finish. Extracted so the loop below is only
// about when this task may leave, not about what it does while it stays.
void DecideOneRequest(Runtime& runtime, Request* request)
{
    const auto is_current = [&runtime, request] {
        return runtime.active_generation.load(std::memory_order_acquire) ==
                   request->transport_generation &&
               esp_timer_get_time() < request->deadline_us;
    };
    const OwnerTrustCommissioningOutcome outcome =
        runtime.commissioner.Commission(
            request->payload, request->transport_generation, is_current);
    if (outcome.code == OwnerTrustCommissioningCode::Staged) {
        request->trust_staged = true;
        request->response = BuildTrustStagedJson(
            outcome.identity.device_instance_id, outcome.owner_domain_id);
        ESP_LOGI(TAG, "Staged Owner Domain %s",
                 outcome.owner_domain_id.c_str());
    } else if (outcome.code == OwnerTrustCommissioningCode::Prepared) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "contract_version", "1");
        cJSON_AddBoolToObject(root, "prepared", true);
        cJSON_AddStringToObject(root, "owner_domain_id", outcome.owner_domain_id.c_str());
        cJSON_AddStringToObject(root, "device_id", outcome.identity.device_instance_id.c_str());
        cJSON_AddStringToObject(root, "identity_fingerprint", outcome.identity.fingerprint.c_str());
        char* raw = cJSON_PrintUnformatted(root);
        if (raw) request->response = raw;
        cJSON_free(raw);
        cJSON_Delete(root);
    } else {
        request->response = BuildTrustRefusedJson(RefusalReason(outcome.code));
        ESP_LOGW(TAG, "Refused Owner trust with code %d",
                 static_cast<int>(outcome.code));
    }

    ESP_LOGI(TAG, "Worker stack spare after trust decision: %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    xSemaphoreGive(request->completed);
    Release(request);  // worker reference
}

// Returns true once this task has released everything it owns and may leave.
bool RetireIfAsked(Runtime& runtime)
{
    if (!runtime.retire_requested.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(runtime.lifecycle);
    // Re-read under the lock: Deactivate() may have stopped waiting since the
    // load above, in which case it is still counting on this task to be here.
    if (!runtime.retire_requested.load(std::memory_order_acquire)) return false;
    ReleaseQueuedRequests(runtime);
    QueueHandle_t queue = runtime.queue;
    runtime.queue = nullptr;
    runtime.task = nullptr;
    runtime.retire_requested.store(false, std::memory_order_release);
    vQueueDelete(queue);
    return true;
}

void ProcessRequests(void*)
{
    Runtime& runtime = State();
    while (true) {
        Request* request = nullptr;
        if (xQueueReceive(runtime.queue, &request, portMAX_DELAY) == pdTRUE &&
            request != nullptr) {
            DecideOneRequest(runtime, request);
        }
        // The safe point, and the only one: nothing in hand, no descriptor half
        // verified, no NVS write open. A nullptr wake is Deactivate() asking
        // whether this task may leave; after a real decision the same question
        // is asked again, because the request that kept the worker alive is now
        // answered.
        if (RetireIfAsked(runtime)) break;
    }
    xSemaphoreGive(runtime.retired);
    // 12288 bytes of internal RAM go back to the heap here. On the boot that ran
    // commissioning that is the difference between a LiveKit engine that can be
    // built and one that cannot.
    vTaskDelete(nullptr);
}

}  // namespace

OwnerTrustCommissioningWorker& OwnerTrustCommissioningWorker::GetInstance()
{
    static OwnerTrustCommissioningWorker worker;
    return worker;
}

esp_err_t OwnerTrustCommissioningWorker::Activate(
    uint32_t transport_generation)
{
    if (transport_generation == 0) return ESP_ERR_INVALID_ARG;
    Runtime& runtime = State();
    std::lock_guard<std::mutex> lock(runtime.lifecycle);
    if (runtime.retired == nullptr) {
        runtime.retired = xSemaphoreCreateBinary();
        if (runtime.retired == nullptr) return ESP_ERR_NO_MEM;
    }
    if (runtime.queue == nullptr) {
        runtime.queue = xQueueCreate(kQueueDepth, sizeof(Request*));
        if (runtime.queue == nullptr) return ESP_ERR_NO_MEM;
    }
    if (runtime.task == nullptr) {
        if (xTaskCreate(
                &ProcessRequests,
                "owner_trust",
                CONFIG_EIDOLON_OWNER_TRUST_WORKER_STACK_BYTES,
                nullptr,
                5,
                &runtime.task) != pdPASS) {
            vQueueDelete(runtime.queue);
            runtime.queue = nullptr;
            return ESP_ERR_NO_MEM;
        }
    } else {
        // A worker that outlived its generation is reused rather than joined by
        // a second one. This is only reachable after a Deactivate() that timed
        // out, and reporting it here is how that failure stays visible past the
        // moment it happened.
        ESP_LOGW(TAG,
                 "Reusing an owner trust worker that did not retire with its "
                 "previous generation; its %u bytes of internal RAM were never "
                 "returned",
                 static_cast<unsigned>(
                     CONFIG_EIDOLON_OWNER_TRUST_WORKER_STACK_BYTES));
    }
    // Adopting a task means it must not act on a retirement asked for by the
    // generation that has just ended. Clearing it here keeps that guarantee
    // local, instead of resting on the order in which Deactivate() gave up.
    runtime.retire_requested.store(false, std::memory_order_release);
    runtime.active_generation.store(transport_generation,
                                    std::memory_order_release);
    return ESP_OK;
}

bool OwnerTrustCommissioningWorker::Deactivate(
    uint32_t transport_generation)
{
    Runtime& runtime = State();
    {
        std::lock_guard<std::mutex> lock(runtime.lifecycle);
        uint32_t expected = transport_generation;
        runtime.active_generation.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
        // Nothing to hand back. Deactivating a generation that never started a
        // worker is not a failure.
        if (runtime.task == nullptr) return true;
        // Clear any signal left by a worker that retired after a previous
        // Deactivate() stopped waiting for it. Without this, that stale give
        // would be mistaken for THIS worker retiring and the caller would be
        // told a task is gone while it is still holding its stack.
        xSemaphoreTake(runtime.retired, 0);
        runtime.retire_requested.store(true, std::memory_order_release);
        // Wake it if it is idle. A full queue means a request is already on its
        // way to the safe point, and the worker asks the same question there.
        Request* wake = nullptr;
        xQueueSend(runtime.queue, &wake, 0);
    }

    // Bounded by the same deadline a Submit() caller waits, because the only
    // thing that can delay retirement is a request still being decided — which
    // that deadline already bounds. An idle worker retires in microseconds, so
    // this costs teardown latency only in a case that has already gone wrong.
    if (xSemaphoreTake(runtime.retired, kResponseWait) == pdTRUE) return true;

    std::lock_guard<std::mutex> lock(runtime.lifecycle);
    if (runtime.task == nullptr) return true;  // retired just as the wait expired
    runtime.retire_requested.store(false, std::memory_order_release);
    // Fail open, deliberately. A resident task costs 12 KiB and this line; a
    // vTaskDelete() landing inside an NVS commit costs a trust store that no
    // amount of rebooting repairs. Refuse to trade the second for the first,
    // and say plainly that the internal RAM did not come back.
    ESP_LOGE(TAG,
             "Owner trust worker did not reach a safe point for generation %lu; "
             "keeping it resident rather than deleting a task that may be "
             "mid-verification. Its %u bytes of internal RAM stay held for this "
             "boot, which can cost the LiveKit control room its engine",
             static_cast<unsigned long>(transport_generation),
             static_cast<unsigned>(
                 CONFIG_EIDOLON_OWNER_TRUST_WORKER_STACK_BYTES));
    return false;
}

esp_err_t OwnerTrustCommissioningWorker::Submit(
    uint32_t transport_generation,
    const uint8_t* payload,
    size_t payload_size,
    std::string& response,
    bool& staged)
{
    response.clear();
    staged = false;
    Runtime& runtime = State();
    // Generation 0 is not a generation — Activate() refuses it — and it is also
    // the value active_generation is parked at once a generation ends. Without
    // this the two zeroes compare equal and a late submission walks straight
    // past a guard that is meant to stop it.
    if (transport_generation == 0 || payload == nullptr || payload_size == 0 ||
        payload_size > kMaxTrustHandoverPayloadBytes ||
        runtime.active_generation.load(std::memory_order_acquire) !=
            transport_generation) {
        response = BuildTrustRefusedJson("handover is not supported");
        return ESP_ERR_INVALID_ARG;
    }

    auto* request = new (std::nothrow) Request(
        std::string(reinterpret_cast<const char*>(payload), payload_size),
        transport_generation);
    if (request == nullptr || request->completed == nullptr) {
        delete request;
        response = BuildTrustRefusedJson("owner trust worker is unavailable");
        return ESP_ERR_NO_MEM;
    }

    Retain(request);  // worker reference, transferred by a successful enqueue
    // The queue handle stays valid only while the worker is alive, and the
    // worker now retires with its generation. Check and send under the lock so
    // a retirement cannot slip between the two, and tell a refused caller which
    // of the two things happened — a busy worker is worth retrying, a departed
    // one is not.
    bool worker_departed = false;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(runtime.lifecycle);
        worker_departed = runtime.queue == nullptr;
        if (!worker_departed) {
            sent = xQueueSend(runtime.queue, &request, 0) == pdTRUE;
        }
    }
    if (!sent) {
        Release(request);  // worker reference was not transferred
        response = BuildTrustRefusedJson(
            worker_departed ? "setup session is no longer active"
                            : "owner trust worker is busy");
        Release(request);  // callback reference
        return worker_departed ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }

    const BaseType_t completed = xSemaphoreTake(request->completed, kResponseWait);
    if (completed == pdTRUE) {
        response = request->response;
        staged = request->trust_staged;
    } else {
        // The worker's shorter commit deadline prevents a late active-marker
        // flip after this terminal refusal is returned.
        response = BuildTrustRefusedJson("owner trust decision timed out");
    }
    Release(request);  // callback reference
    return completed == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

}  // namespace eidolon
