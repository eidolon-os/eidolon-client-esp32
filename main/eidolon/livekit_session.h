#ifndef EIDOLON_LIVEKIT_SESSION_H_
#define EIDOLON_LIVEKIT_SESSION_H_

#include <esp_err.h>
#include <functional>
#include <string>
#include <array>
#include <mutex>
#include <vector>

#include <livekit.h>
#include <livekit_data_stream.h>

#include "eidolon_ui_types.h"
#include "hub_types.h"
#include "session_memory_admission_core.h"
#include "pending_session_control.h"
#include "transcription_stream.h"

namespace eidolon {

enum class LiveKitConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed,
};

class LiveKitSession {
public:
    using StateCallback = std::function<void(LiveKitConnectionState, uint32_t generation)>;
    using ConfigurationCallback = std::function<void(uint32_t generation)>;
    using TranscriptionCallback = std::function<void(const TranscriptionEvent& event)>;
    using AgentPlaybackCallback = std::function<void(bool speaking, uint32_t generation)>;
    using AgentPhaseCallback = std::function<void(AgentPhase phase)>;
    using ControlCommandCallback = std::function<void(const std::string& payload, uint32_t generation, bool agent, bool provider)>;
    using SessionControlCallback = std::function<void(const std::string& payload, uint32_t generation, bool agent)>;
    using DeviceEventCallback =
        std::function<void(const std::string& payload, uint32_t generation)>;
    // A call on one of the device's RPC methods (device_settings_core). The
    // transport only classifies the caller the way it classifies control
    // packets and hands the call over; the controller answers from its own task.
    struct RpcCall {
        std::string id;
        std::string method;
        std::string payload;
        bool agent = false;
        bool provider = false;
        uint32_t generation = 0;
        // The participant that called: whoever a still is sent back to.
        std::string caller_identity;
    };
    // Returns whether the call was taken; a refused one is answered here.
    using RpcCallback = std::function<bool(RpcCall call)>;

    esp_err_t Connect(const Esp32HubConfig& config, uint32_t generation);

    esp_err_t Disconnect();

    // Same-channel credential renewal; the engine owns reconnect serialization.
    esp_err_t UpdateConnectionToken(const std::string& expected, const std::string& token);

    // True once repeated Connect() attempts have been refused for internal RAM
    // with the shortfall not moving. Callers own retry policy, and this is the
    // fact that tells them a retry is not a retry — it is the same measurement
    // again. Cleared by the next room that does get built.
    bool InternalMemoryCeilingReached() const { return memory_ledger_.ceiling_reached(); }
    std::size_t InternalMemoryShortfallBytes() const
    {
        return memory_ledger_.last_binding_shortfall();
    }
    const char* InternalMemoryVerdictName() const
    {
        return SessionMemoryVerdictName(memory_ledger_.last_verdict());
    }

    // An edge that legitimately changes the device's situation — network
    // restored, re-activation, a person asking for a conversation — earns one
    // more look at the heap. Without this the ceiling would be permanent for the
    // uptime, which trades a flapping retry loop for a device that has quietly
    // stopped trying.
    void ForgetInternalMemoryCeiling() { memory_ledger_.Reset(); }

    bool HasRoom() const { return room_handle_ != nullptr; }
    bool IsConnected() const;
    livekit_failure_reason_t LastFailureReason() const { return last_failure_reason_; }
    esp_err_t PublishData(const std::string& topic, const std::string& payload,
                          bool reliable = true);
    // One byte stream to one participant of the room `generation` names:
    // header with these attributes, the bytes, trailer. Refused when that room
    // is gone or replaced. Call from the controller task, like PublishData.
    struct StreamAttribute {
        const char* key;
        std::string value;
    };
    esp_err_t SendByteStream(uint32_t generation, const char* topic, const std::string& destination,
                             const std::vector<StreamAttribute>& attributes, const uint8_t* data,
                             size_t size);

    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnConfigurationInvalidated(ConfigurationCallback cb) { on_configuration_invalidated_ = std::move(cb); }
    void SetOnTranscription(TranscriptionCallback cb) { on_transcription_ = std::move(cb); }
    void SetOnAgentPlayback(AgentPlaybackCallback cb) { on_agent_playback_ = std::move(cb); }
    void SetOnAgentPhase(AgentPhaseCallback cb) { on_agent_phase_ = std::move(cb); }
    void SetOnControlCommand(ControlCommandCallback cb) { on_control_command_ = std::move(cb); }
    void SetOnSessionControl(SessionControlCallback cb) { on_session_control_ = std::move(cb); }
    void SetOnDeviceEvent(DeviceEventCallback cb) { on_device_event_ = std::move(cb); }
    void SetOnRpc(RpcCallback cb) { on_rpc_ = std::move(cb); }
    // Answer one call: `error_code` 0 sends `text` as the payload, anything
    // else as the error. Dropped when the room the call arrived on is gone or
    // replaced, because the SDK's sender belongs to that room.
    void RespondRpc(uint32_t generation, const std::string& id, int error_code,
                    const std::string& text);

private:
    static void OnRoomStateChanged(livekit_connection_state_t state, void* ctx);
    TranscriptionStream transcription_stream_;
    TranscriptionSource transcription_source_ = TranscriptionSource::Unknown;
    static void OnTextStreamOpen(const livekit_data_stream_header_t* header, void* ctx);
    static void OnTextStreamClose(const livekit_data_stream_trailer_t* trailer, void* ctx);
    static void OnTextStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx);
    static void OnDrainStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx);
    static void OnParticipantInfo(const livekit_participant_info_t* info, void* ctx);
    bool IsAgent(const char* identity);
    std::mutex peers_mutex_;
    std::array<std::string, 4> agent_peers_;
    PendingSessionControl pending_session_control_;
    static void OnDataReceived(const livekit_data_received_t* data, void* ctx);
    // The SDK passes RPC handlers no context, and this device holds one room.
    static void OnRpcInvocation(const livekit_rpc_invocation_t* invocation, void* ctx);
    static LiveKitSession* rpc_session_;
    void RegisterRpcMethods();
    RpcCallback on_rpc_;
    std::mutex rpc_mutex_;
    bool (*rpc_send_)(const livekit_rpc_result_t* result, void* ctx) = nullptr;
    void* rpc_ctx_ = nullptr;

    void HandleStateChanged(livekit_connection_state_t state);
    void HandleUiStatePayload(const char* payload, size_t size);
    void RegisterTranscriptionHandler();
    void RegisterAgentSessionDrainHandler();
    void UnregisterStreamHandlers();
    // There is one kind of board now. The device used to keep two — a mic-free
    // one for the room it lived in and a codec+AFE one for the room it visited
    // — so the start of every conversation tore one down and built the other,
    // which is the moment a memory-tight board had least room to build it in.
    // Built once and kept, because the channel it serves is kept.
    esp_err_t EnsureMediaBoard(bool audio_input, bool audio_output);
    void ReleaseMediaBoard();
    // Decides, and says in the log, whether the internal heap can hold a room
    // right now. Returns false having already reported why.
    bool AdmitEngineMemory();

    livekit_room_handle_t room_handle_ = nullptr;
    std::string identity_;
    std::string provider_identity_;
    bool connected_ = false;
    bool using_media_ = false;
    bool media_board_initialized_ = false;
    bool transcription_registered_ = false;
    bool agent_session_registered_ = false;
    livekit_failure_reason_t last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    SessionMemoryRetryLedger memory_ledger_;
    uint32_t generation_ = 0;
    StateCallback on_state_changed_;
    ConfigurationCallback on_configuration_invalidated_;
    TranscriptionCallback on_transcription_;
    AgentPhaseCallback on_agent_phase_;
    AgentPlaybackCallback on_agent_playback_;
    ControlCommandCallback on_control_command_;
    SessionControlCallback on_session_control_;
    DeviceEventCallback on_device_event_;
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_SESSION_H_
