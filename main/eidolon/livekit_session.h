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
    using DeviceEventCallback =
        std::function<void(const std::string& payload, uint32_t generation)>;

    esp_err_t Connect(const Esp32HubConfig& config, uint32_t generation);

    // Replace the transport session on this channel, keeping the channel — its
    // credentials, its room, and the media board — exactly as it is.
    //
    // REMOVABLE. This exists for one reason: the LiveKit ESP client records the
    // single remote audio track a connection is subscribed to, and releases
    // that record only when the connection itself is torn down. Nothing clears
    // it when the track goes away. So a connection that has carried one
    // conversation believes forever that it is still subscribed to a track that
    // no longer exists, and the next agent's audio is refused with
    // ENGINE_ERR_MAX_SUB — the device sits in a conversation it cannot hear.
    //
    // The client is a stated developer preview; LiveKit's own model everywhere
    // else is one long connection carrying tracks that come and go. When this
    // client grows that half, delete this method and its caller: nothing else
    // depends on the session being replaced.
    //
    // The two-room design that came before never met this, because switching
    // rooms gave every conversation a new connection anyway. This keeps that
    // one load-bearing property and drops what it used to cost — a second room,
    // a second credential, and a media board rebuilt per conversation.
    esp_err_t RenewSession(const Esp32HubConfig& config, uint32_t generation);

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

    livekit_room_handle_t room_handle_ = nullptr;
    std::string identity_;
    bool connected_ = false;
    bool using_media_ = false;
    bool media_board_initialized_ = false;
    bool transcription_registered_ = false;
    bool agent_session_registered_ = false;
    livekit_failure_reason_t last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
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
