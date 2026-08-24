#pragma once

#include <cstddef>

namespace eidolon {

// Network restore is serialized on the controller actor and includes Owner
// directory reload, signed configuration proof, HTTPS and channel reconnect.
// Box3 HIL proved that the complete path exhausts the former 8 KiB budget.
// Keep the actor boundary and reserve the measured crypto/control-plane stack
// instead of moving blocking work back into callbacks or retrying a crash.
inline constexpr std::size_t kControllerWorkerStackBytes = 16 * 1024;

}  // namespace eidolon
