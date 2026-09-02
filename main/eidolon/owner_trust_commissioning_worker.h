#ifndef EIDOLON_OWNER_TRUST_COMMISSIONING_WORKER_H_
#define EIDOLON_OWNER_TRUST_COMMISSIONING_WORKER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_err.h>

namespace eidolon {

// FreeRTOS adapter for the Host-independent OwnerTrustCommissioner. Protocomm
// callbacks submit one bounded copy and wait only for the protocol response;
// parsing, X.509/ES256 work and durable storage all run on this single writer.
class OwnerTrustCommissioningWorker {
public:
    static OwnerTrustCommissioningWorker& GetInstance();

    esp_err_t Activate(uint32_t transport_generation);
    // Asks the worker to retire and waits, bounded, for it to reach a safe
    // point. Returns false if it did not: the task is then still resident and
    // its internal RAM has NOT come back, which the caller must not record as a
    // completed release. Retirement is cooperative because this task is often
    // inside X.509 verification or an NVS commit, where an external delete
    // would cost far more than the stack it reclaims.
    bool Deactivate(uint32_t transport_generation);

    esp_err_t Submit(uint32_t transport_generation,
                     const uint8_t* payload,
                     size_t payload_size,
                     std::string& response,
                     bool& staged);

private:
    OwnerTrustCommissioningWorker() = default;
};

}  // namespace eidolon

#endif  // EIDOLON_OWNER_TRUST_COMMISSIONING_WORKER_H_
