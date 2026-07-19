#include "livekit_session.h"

#include "eidolon_topics.h"
#include "livekit_data_only_compat.h"
#include "livekit_board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <livekit.h>

#include <cstring>

#define TAG "LiveKitSession"

namespace eidolon {

namespace {

LiveKitConnectionState MapConnectionState(livekit_connection_state_t state)
{
    switch (state) {
    case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
        return LiveKitConnectionState::Disconnected;
    case LIVEKIT_CONNECTION_STATE_CONNECTING:
        return LiveKitConnectionState::Connecting;
    case LIVEKIT_CONNECTION_STATE_CONNECTED:
        return LiveKitConnectionState::Connected;
    case LIVEKIT_CONNECTION_STATE_RECONNECTING:
        return LiveKitConnectionState::Reconnecting;
    case LIVEKIT_CONNECTION_STATE_FAILED:
        return LiveKitConnectionState::Failed;
    default:
        return LiveKitConnectionState::Disconnected;
    }
}

const char* JsonString(cJSON* root, const char* key)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool JsonBool(cJSON* root, const char* key, bool fallback)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

const char* NestedJsonString(cJSON* root, const char* object_key, const char* key)
{
    cJSON* object = cJSON_GetObjectItem(root, object_key);
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }
    return JsonString(object, key);
}

TranscriptionSource SourceFromString(const char* value)
{
    if (!value) {
        return TranscriptionSource::Unknown;
    }
    if (strcmp(value, "user") == 0 || strcmp(value, "local") == 0) {
        return TranscriptionSource::User;
    }
    if (strcmp(value, "agent") == 0 || strcmp(value, "assistant") == 0 ||
        strcmp(value, "remote") == 0) {
        return TranscriptionSource::Agent;
    }
    if (strcmp(value, "system") == 0) {
        return TranscriptionSource::System;
    }
    return TranscriptionSource::Unknown;
}

AgentPhase PhaseFromUiState(const char* value, const char* reason)
{
    if (!value || strcmp(value, "idle") == 0) {
        return AgentPhase::Silent;
    }
    if (strcmp(value, "listening") == 0) {
        return (reason && strcmp(reason, "user_state:speaking") == 0)
                   ? AgentPhase::UserSpeaking
                   : AgentPhase::Silent;
    }
    if (strcmp(value, "user_speaking") == 0) {
        return AgentPhase::UserSpeaking;
    }
    if (strcmp(value, "thinking") == 0 || strcmp(value, "processing") == 0) {
        return AgentPhase::AgentThinking;
    }
    if (strcmp(value, "speaking") == 0 || strcmp(value, "agent_speaking") == 0) {
        return AgentPhase::AgentSpeaking;
    }
    return AgentPhase::Silent;
}

}  // namespace

void LiveKitSession::OnRoomStateChanged(livekit_connection_state_t state, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (session) {
        session->HandleStateChanged(state);
    }
}

void LiveKitSession::OnTextStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !chunk || !session->on_transcription_ || chunk->content_size == 0) {
        return;
    }

    std::string payload(reinterpret_cast<const char*>(chunk->content), chunk->content_size);
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        // lk.transcription may deliver raw token/chunk text. Those chunks are
        // transport data, not a stable device subtitle; only structured
        // transcription payloads are surfaced to the UI.
        return;
    }

    const char* text = JsonString(root, "text");
    if (!text) {
        text = JsonString(root, "transcript");
    }
    if (!text) {
        text = JsonString(root, "content");
    }
    if (text && text[0] != '\0') {
        const char* source = JsonString(root, "source");
        if (!source) {
            source = JsonString(root, "role");
        }

        TranscriptionSource mapped_source = SourceFromString(source);
        if (mapped_source == TranscriptionSource::Unknown) {
            const char* identity = JsonString(root, "participant_identity");
            if (!identity) {
                identity = JsonString(root, "identity");
            }
            if (!identity) {
                identity = NestedJsonString(root, "participant", "identity");
            }
            if (identity && identity[0] != '\0') {
                mapped_source = (session->identity_ == identity)
                                    ? TranscriptionSource::User
                                    : TranscriptionSource::Agent;
            }
        }

        bool is_final = JsonBool(root, "final", JsonBool(root, "is_final", false));
        session->on_transcription_({mapped_source, text, is_final});
    }
    cJSON_Delete(root);
}

void LiveKitSession::OnDrainStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx)
{
    (void)chunk;
    (void)ctx;
}

void LiveKitSession::OnDataReceived(const livekit_data_received_t* data, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !data || !data->payload.bytes || data->payload.size == 0) {
        return;
    }
    const char* topic = data->topic ? data->topic : "";
    if (strcmp(topic, kUiStateTopic) == 0) {
        session->HandleUiStatePayload(reinterpret_cast<const char*>(data->payload.bytes),
                                      data->payload.size);
        return;
    }
    if (strcmp(topic, kSessionControlTopic) == 0 && session->on_session_control_) {
        std::string payload(reinterpret_cast<const char*>(data->payload.bytes),
                            data->payload.size);
        ESP_LOGI(TAG, "[lifecycle] session_control received topic=%s bytes=%u",
                 topic, static_cast<unsigned>(data->payload.size));
        session->on_session_control_(payload);
        return;
    }
    if (strcmp(topic, kControlTopic) != 0 || !session->on_control_command_) {
        return;
    }
    std::string payload(reinterpret_cast<const char*>(data->payload.bytes), data->payload.size);
    session->on_control_command_(payload);
}

void LiveKitSession::HandleStateChanged(livekit_connection_state_t state)
{
    auto mapped = MapConnectionState(state);
    connected_ = (mapped == LiveKitConnectionState::Connected);
    if (mapped == LiveKitConnectionState::Connected) {
        last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    }
    // room_kind is inferred from whether this connection publishes/subscribes
    // media (voice) or is data-only (control), so the SDK-level transition lines
    // align with the controller's [lifecycle] logs by identity + room_kind.
    ESP_LOGI(TAG, "[lifecycle] room state=%s room_kind=%s identity=%s gen=%lu",
             livekit_connection_state_str(state), using_media_ ? "voice" : "control",
             identity_.c_str(), static_cast<unsigned long>(generation_));

    if ((mapped == LiveKitConnectionState::Failed ||
         mapped == LiveKitConnectionState::Reconnecting) &&
        room_handle_ != nullptr) {
        livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle_);
        last_failure_reason_ = reason;
        if (reason != LIVEKIT_FAILURE_REASON_NONE) {
            ESP_LOGE(TAG, "Failure: %s", livekit_failure_reason_str(reason));
        }
    }

    if (on_state_changed_) {
        on_state_changed_(mapped, generation_);
    }
}

void LiveKitSession::HandleUiStatePayload(const char* payload, size_t size)
{
    if (!on_agent_phase_ || !payload || size == 0) {
        return;
    }
    std::string json(payload, size);
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        return;
    }
    const char* state = JsonString(root, "state");
    if (!state) {
        state = JsonString(root, "phase");
    }
    if (state) {
        on_agent_phase_(PhaseFromUiState(state, JsonString(root, "reason")));
    }
    cJSON_Delete(root);
}

void LiveKitSession::RegisterTranscriptionHandler()
{
    if (!room_handle_ || !on_transcription_) {
        return;
    }
    livekit_data_stream_handler_t handler = {
        .on_recv = OnTextStreamChunk,
        .ctx = this,
    };
    livekit_room_data_stream_topic_register(room_handle_, kTranscriptionTopic, &handler);
    transcription_registered_ = true;
}

void LiveKitSession::RegisterAgentSessionDrainHandler()
{
    if (!room_handle_) {
        return;
    }
    livekit_data_stream_handler_t handler = {
        .on_recv = OnDrainStreamChunk,
        .ctx = this,
    };
    livekit_room_data_stream_topic_register(room_handle_, kAgentSessionTopic, &handler);
    agent_session_registered_ = true;
}

void LiveKitSession::UnregisterStreamHandlers()
{
    if (!room_handle_) {
        transcription_registered_ = false;
        agent_session_registered_ = false;
        return;
    }
    if (transcription_registered_) {
        livekit_room_data_stream_topic_unregister(room_handle_, kTranscriptionTopic);
        transcription_registered_ = false;
    }
    if (agent_session_registered_) {
        livekit_room_data_stream_topic_unregister(room_handle_, kAgentSessionTopic);
        agent_session_registered_ = false;
    }
}

esp_err_t LiveKitSession::EnsureMediaBoard(bool data_only)
{
    if (media_board_initialized_) {
        return ESP_OK;
    }
    esp_err_t err = data_only ? eidolon_livekit_board_init_data_only()
                              : eidolon_livekit_board_init();
    if (err == ESP_OK) {
        media_board_initialized_ = true;
    } else {
        // board_init can fail after constructing only part of the provider.
        // Always return it to a clean state so the next bounded retry starts
        // from a complete capturer/renderer pair.
        eidolon_livekit_board_deinit();
    }
    return err;
}

void LiveKitSession::ReleaseMediaBoard()
{
    if (!media_board_initialized_) {
        return;
    }
    eidolon_livekit_board_deinit();
    media_board_initialized_ = false;
}

esp_err_t LiveKitSession::Connect(const Esp32HubConfig& config, uint32_t generation)
{
    if (room_handle_ != nullptr) {
        Disconnect(true);
    }

    identity_ = config.active.identity;
    generation_ = generation;

    esp_err_t media_err = EnsureMediaBoard(/*data_only=*/false);
    if (media_err != ESP_OK) {
        return media_err;
    }

    esp_capture_handle_t capturer = eidolon_livekit_board_get_capturer();
    av_render_handle_t renderer = eidolon_livekit_board_get_renderer();
    if (!capturer || !renderer) {
        ESP_LOGE(TAG, "Media pipeline not ready");
        ReleaseMediaBoard();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t sample_rate = config.sample_rate > 0 ? config.sample_rate : 16000;
    uint8_t channels = config.channels > 0 ? config.channels : 1;

    livekit_room_options_t room_options = {};
    room_options.publish = {
        .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
        .audio_encode =
            {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = sample_rate,
                .channel_count = channels,
            },
        .capturer = capturer,
    };
    room_options.subscribe = {
        .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
        .renderer = renderer,
    };
    room_options.on_state_changed = OnRoomStateChanged;
    room_options.on_data_received = OnDataReceived;
    room_options.ctx = this;

    if (livekit_room_create(&room_handle_, &room_options) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_create failed");
        room_handle_ = nullptr;
        transcription_registered_ = false;
        agent_session_registered_ = false;
        return ESP_FAIL;
    }

    RegisterTranscriptionHandler();
    RegisterAgentSessionDrainHandler();
    using_media_ = true;

    ESP_LOGI(TAG, "Connecting room=%s identity=%s server=%s", config.active.room_name.c_str(),
             config.active.identity.c_str(), config.active.server_url.c_str());

    if (livekit_room_connect(room_handle_, config.active.server_url.c_str(),
                             config.active.token.c_str()) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_connect failed");
        UnregisterStreamHandlers();
        livekit_room_destroy(room_handle_);
        room_handle_ = nullptr;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t LiveKitSession::ConnectDataOnly(const Esp32HubConfig& config, uint32_t generation)
{
    if (room_handle_ != nullptr) {
        Disconnect(true);
    }

    identity_ = config.active.identity;
    generation_ = generation;

    esp_err_t media_err = EnsureMediaBoard(/*data_only=*/true);
    if (media_err != ESP_OK) {
        return media_err;
    }

    esp_capture_handle_t capturer = eidolon_livekit_board_get_capturer();
    if (!capturer) {
        ESP_LOGE(TAG, "Control-room capturer not ready");
        ReleaseMediaBoard();
        return ESP_ERR_INVALID_STATE;
    }

    // LiveKit 0.3.10 forces a working capture sink on every room, so the control room
    // publishes a MIC-FREE silent Opus track (fed by the silent capturer). The
    // data-only token (can_publish=false) denies the publish server-side, so the room
    // stays effectively data-only and carries only data packets. subscribe=NONE:
    // control needs no playback path.
    livekit_room_options_t room_options = {};
    room_options.publish = {
        .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
        .audio_encode =
            {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel_count = 1,
            },
        .capturer = capturer,
    };
    room_options.subscribe = {
        .kind = LIVEKIT_MEDIA_TYPE_NONE,
    };
    room_options.on_state_changed = OnRoomStateChanged;
    room_options.on_data_received = OnDataReceived;
    room_options.ctx = this;
    using_media_ = false;

    if (livekit_room_create(&room_handle_, &room_options) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_create data-only failed");
        room_handle_ = nullptr;
        transcription_registered_ = false;
        agent_session_registered_ = false;
        ReleaseMediaBoard();
        return ESP_FAIL;
    }
    transcription_registered_ = false;
    agent_session_registered_ = false;

    ESP_LOGI(TAG, "Connecting control room=%s identity=%s server=%s",
             config.active.room_name.c_str(), config.active.identity.c_str(),
             config.active.server_url.c_str());

    if (livekit_room_connect(room_handle_, config.active.server_url.c_str(),
                             config.active.token.c_str()) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_connect data-only failed");
        livekit_room_destroy(room_handle_);
        room_handle_ = nullptr;
        transcription_registered_ = false;
        agent_session_registered_ = false;
        ReleaseMediaBoard();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t LiveKitSession::Disconnect(bool release_media)
{
    connected_ = false;
    if (room_handle_ == nullptr) {
        identity_.clear();
        last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
        if (release_media && media_board_initialized_) {
            ESP_LOGI(TAG, "Releasing LiveKit media board without active room");
            ReleaseMediaBoard();
            using_media_ = false;
        }
        return ESP_OK;
    }

    livekit_room_handle_t handle = room_handle_;
    ESP_LOGI(TAG, "Disconnecting room (release_media=%d)", release_media ? 1 : 0);

    UnregisterStreamHandlers();

    if (livekit_room_close(handle) != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "livekit_room_close failed");
    }

    bool disconnected = false;
    for (int i = 0; i < 30; ++i) {
        if (livekit_room_get_state(handle) == LIVEKIT_CONNECTION_STATE_DISCONNECTED) {
            disconnected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!disconnected) {
        ESP_LOGW(TAG, "Timed out waiting for LiveKit room to disconnect");
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    if (livekit_room_destroy(handle) != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "livekit_room_destroy failed");
    }
    room_handle_ = nullptr;
    identity_.clear();
    last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    if (release_media && media_board_initialized_) {
        ReleaseMediaBoard();
        ESP_LOGI(TAG, "LiveKit media board released");
        using_media_ = false;
    }
    return ESP_OK;
}

bool LiveKitSession::IsConnected() const
{
    return connected_;
}

esp_err_t LiveKitSession::PublishData(const std::string& topic, const std::string& payload,
                                      bool reliable)
{
    if (!room_handle_ || !connected_) {
        return ESP_ERR_INVALID_STATE;
    }
    livekit_data_payload_t data = {
        .bytes = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data())),
        .size = payload.size(),
    };
    livekit_data_publish_options_t options = {
        .payload = &data,
        .topic = const_cast<char*>(topic.c_str()),
        .lossy = !reliable,
        .destination_identities = nullptr,
        .destination_identities_count = 0,
    };
    if (livekit_room_publish_data(room_handle_, &options) != LIVEKIT_ERR_NONE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace eidolon
