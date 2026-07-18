#ifndef EIDOLON_LIVEKIT_SESSION_H_
#define EIDOLON_LIVEKIT_SESSION_H_

#include <esp_err.h>
#include <functional>
#include <string>

#include <livekit.h>
#include <livekit_data_stream.h>

#include "eidolon_ui_types.h"
#include "hub_types.h"

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

    esp_err_t Connect(const Esp32HubConfig& config, uint32_t generation);
    esp_err_t ConnectDataOnly(const Esp32HubConfig& config, uint32_t generation);
    esp_err_t Disconnect(bool release_media = true);
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

    livekit_room_handle_t room_handle_ = nullptr;
    std::string identity_;
    bool connected_ = false;
    bool using_media_ = false;
    bool transcription_registered_ = false;
    bool agent_session_registered_ = false;
    livekit_failure_reason_t last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    uint32_t generation_ = 0;
    StateCallback on_state_changed_;
    TranscriptionCallback on_transcription_;
    AgentPhaseCallback on_agent_phase_;
    ControlCommandCallback on_control_command_;
    SessionControlCallback on_session_control_;
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_SESSION_H_
