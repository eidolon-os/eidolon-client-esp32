#ifndef EIDOLON_LIVEKIT_SESSION_H_
#define EIDOLON_LIVEKIT_SESSION_H_

#include <esp_err.h>
#include <functional>
#include <string>

#include <livekit.h>
#include <livekit_data_stream.h>

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
    using StateCallback = std::function<void(LiveKitConnectionState)>;
    using TranscriptionCallback = std::function<void(const std::string& text)>;

    esp_err_t Connect(const Esp32HubConfig& config);
    esp_err_t Disconnect();
    bool IsConnected() const;

    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnTranscription(TranscriptionCallback cb) { on_transcription_ = std::move(cb); }

private:
    static void OnRoomStateChanged(livekit_connection_state_t state, void* ctx);
    static void OnTextStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx);

    void HandleStateChanged(livekit_connection_state_t state);
    void RegisterTranscriptionHandler();

    livekit_room_handle_t room_handle_ = nullptr;
    bool connected_ = false;
    StateCallback on_state_changed_;
    TranscriptionCallback on_transcription_;
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_SESSION_H_
