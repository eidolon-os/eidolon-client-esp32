#ifndef EIDOLON_SMARTHOME_WIRE_H_
#define EIDOLON_SMARTHOME_WIRE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "smarthome_model.h"

// JSON codec for the panel wire. Downward messages arrive as the payload of an
// eidolon.control command (ControlCommand::op / ::payload); upward payloads are
// what the panel hands its transport for smarthome.execute / smarthome.sync.
namespace eidolon::smarthome {

inline constexpr const char* kOpSnapshot = "smarthome.snapshot";
inline constexpr const char* kOpDelta = "smarthome.delta";
inline constexpr const char* kOpResult = "smarthome.result";
// Upward requests travel on their own LiveKit data topic, as a request body
// (SDK PanelRequest), never as device events.
inline constexpr const char* kPanelRequestTopic = "eidolon.smarthome";
inline constexpr const char* kRequestExecute = "smarthome.execute";
inline constexpr const char* kRequestSync = "smarthome.sync";
inline constexpr const char* kPanelProfileProperty = "smarthome.profile";
inline constexpr const char* kPanelProfile = "smarthome.panel.v1";
inline constexpr int kSchemaVersion = 1;
inline constexpr int kCapabilityVersion = 1;

enum class MessageKind : uint8_t { Snapshot, Delta, Result };

struct Message {
    MessageKind kind = MessageKind::Snapshot;
    Snapshot snapshot;  // kind == Snapshot
    Delta delta;        // kind == Delta
    VoiceResult result; // kind == Result
};

bool IsSmartHomeOp(const std::string& op);

// Parses one control payload. Returns false, leaving `out` unspecified, when
// the op is not a smart home op or the payload breaks the v1 contract; `error`
// then names the first violation. Unknown extra fields are ignored.
bool ParseMessage(const std::string& op, const std::string& payload_json, Message& out,
                  std::string* error = nullptr);

// SDK Identifier: 1..128 of [A-Za-z0-9._:-].
bool IsIdentifier(const std::string& value);

// Full request bodies for kPanelRequestTopic:
//   {"schema_v":1,"type":"smarthome.execute"|"smarthome.sync","payload":{...}}
// Each returns an empty string when the request cannot be expressed on the
// wire (bad identifiers, 0 or more than kMaxPanelCommands commands).
//
// PanelExecute for device commands (scene_id null).
std::string BuildExecuteRequest(const std::string& request_id, const std::vector<Command>& commands);
// PanelExecute for a scene (commands empty); the host expands the scene.
std::string BuildSceneRequest(const std::string& request_id, const std::string& scene_id);
// PanelSync. Without a held snapshot both known fields are null, which asks
// the host for a snapshot unconditionally.
std::string BuildSyncRequest(bool has_snapshot, uint64_t known_revision, uint64_t known_seq);

// Touch request ids double as the host's idempotency key within one Owner, so
// they must not repeat across reboots: the boot nonce has to come from a real
// random source (esp_random on the device).
class RequestIdSource {
public:
    explicit RequestIdSource(uint32_t boot_nonce) : nonce_(boot_nonce) {}
    std::string Next();

private:
    uint32_t nonce_;
    uint32_t counter_ = 0;
};

}  // namespace eidolon::smarthome

#endif  // EIDOLON_SMARTHOME_WIRE_H_
