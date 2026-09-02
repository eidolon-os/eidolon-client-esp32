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
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<uint32_t> active_generation{0};
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
    case OwnerTrustCommissioningCode::Staged:
        break;
    }
    return "owner trust was refused";
}

void ProcessRequests(void*)
{
    Runtime& runtime = State();
    while (true) {
        Request* request = nullptr;
        if (xQueueReceive(runtime.queue, &request, portMAX_DELAY) != pdTRUE ||
            request == nullptr) {
            continue;
        }

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
            auto& identity = DeviceIdentity::GetInstance();
            if (identity.EnsureKeypair() != ESP_OK ||
                identity.DeviceInstanceId().empty()) {
                request->trust_staged = false;
                request->response = BuildTrustRefusedJson(
                    "operational identity is unavailable");
            } else {
                request->response = BuildTrustStagedJson(
                    identity.DeviceInstanceId(), outcome.owner_domain_id);
            }
            ESP_LOGI(TAG, "Staged Owner Domain %s",
                     outcome.owner_domain_id.c_str());
        } else {
            request->response = BuildTrustRefusedJson(
                RefusalReason(outcome.code));
            ESP_LOGW(TAG, "Refused Owner trust with code %d",
                     static_cast<int>(outcome.code));
        }

        ESP_LOGI(TAG, "Worker stack spare after trust decision: %u bytes",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        xSemaphoreGive(request->completed);
        Release(request);  // worker reference
    }
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
    }
    runtime.active_generation.store(transport_generation,
                                    std::memory_order_release);
    return ESP_OK;
}

void OwnerTrustCommissioningWorker::Deactivate(
    uint32_t transport_generation)
{
    Runtime& runtime = State();
    uint32_t expected = transport_generation;
    runtime.active_generation.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel);
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
    if (payload == nullptr || payload_size == 0 ||
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
    if (xQueueSend(runtime.queue, &request, 0) != pdTRUE) {
        Release(request);  // worker reference was not transferred
        response = BuildTrustRefusedJson("owner trust worker is busy");
        Release(request);  // callback reference
        return ESP_ERR_TIMEOUT;
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
