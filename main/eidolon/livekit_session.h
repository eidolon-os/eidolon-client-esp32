#ifndef EIDOLON_LIVEKIT_SESSION_H_
#define EIDOLON_LIVEKIT_SESSION_H_

#include <esp_err.h>
#include <functional>
#include <string>

#include <livekit.h>
#include <livekit_data_stream.h>

#include "eidolon_ui_types.h"
#include "hub_types.h"
#include "session_memory_admission_core.h"

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
    using TranscriptionCallback = std::function<void(const TranscriptionEvent& event)>;
    using AgentPhaseCallback = std::function<void(AgentPhase phase)>;
    using ControlCommandCallback = std::function<void(const std::string& payload)>;
    using SessionControlCallback = std::function<void(const std::string& payload)>;
    using DeviceEventCallback =
        std::function<void(const std::string& payload, uint32_t generation)>;

    esp_err_t Connect(const Esp32HubConfig& config, uint32_t generation);

    esp_err_t Disconnect();

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

    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnTranscription(TranscriptionCallback cb) { on_transcription_ = std::move(cb); }
    void SetOnAgentPhase(AgentPhaseCallback cb) { on_agent_phase_ = std::move(cb); }
    void SetOnControlCommand(ControlCommandCallback cb) { on_control_command_ = std::move(cb); }
    void SetOnSessionControl(SessionControlCallback cb) { on_session_control_ = std::move(cb); }
    void SetOnDeviceEvent(DeviceEventCallback cb) { on_device_event_ = std::move(cb); }

private:
    static void OnRoomStateChanged(livekit_connection_state_t state, void* ctx);
    static void OnTextStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx);
    static void OnDrainStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx);
    static void OnDataReceived(const livekit_data_received_t* data, void* ctx);

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
    esp_err_t EnsureMediaBoard();
    void ReleaseMediaBoard();
    // Decides, and says in the log, whether the internal heap can hold a room
    // right now. Returns false having already reported why.
    bool AdmitEngineMemory();

    livekit_room_handle_t room_handle_ = nullptr;
    std::string identity_;
    bool connected_ = false;
    bool using_media_ = false;
    bool media_board_initialized_ = false;
    bool transcription_registered_ = false;
    bool agent_session_registered_ = false;
    livekit_failure_reason_t last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    SessionMemoryRetryLedger memory_ledger_;
    uint32_t generation_ = 0;
    StateCallback on_state_changed_;
    TranscriptionCallback on_transcription_;
    AgentPhaseCallback on_agent_phase_;
    ControlCommandCallback on_control_command_;
    SessionControlCallback on_session_control_;
    DeviceEventCallback on_device_event_;
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_SESSION_H_
