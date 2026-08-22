#ifndef EIDOLON_COMMISSIONING_RUNTIME_H_
#define EIDOLON_COMMISSIONING_RUNTIME_H_

#include <cstdint>

namespace eidolon {

// ESP-IDF composition root for the platform-independent commissioning Core.
// Board, timer and SDK callbacks only submit events here; the dedicated actor
// is the sole component that advances a setup generation and owns its leases.
class CommissioningRuntime {
public:
    static CommissioningRuntime& GetInstance();

    bool RequestOpen();
    bool RequestCancel();
    bool IsAdvertising() const;
    // True for the whole physical commissioning lease, including identity
    // preparation, transport startup and restoration. Network callbacks use
    // this projection only to avoid presenting an expected Station handover as
    // an unrelated offline failure; they never advance commissioning state.
    bool IsInProgress() const;

private:
    CommissioningRuntime() = default;
};

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_RUNTIME_H_
