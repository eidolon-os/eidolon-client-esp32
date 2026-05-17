#include "livekit_session.h"

#include "livekit_board.h"

#include <cJSON.h>
#include <esp_log.h>
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
        session->on_transcription_(payload);
        return;
    }
    cJSON* text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text)) {
        session->on_transcription_(text->valuestring);
    }
    cJSON_Delete(root);
}

void LiveKitSession::HandleStateChanged(livekit_connection_state_t state)
{
    auto mapped = MapConnectionState(state);
    connected_ = (mapped == LiveKitConnectionState::Connected);
    ESP_LOGI(TAG, "Room state: %s", livekit_connection_state_str(state));

    if (mapped == LiveKitConnectionState::Failed || mapped == LiveKitConnectionState::Reconnecting) {
        livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle_);
        if (reason != LIVEKIT_FAILURE_REASON_NONE) {
            ESP_LOGE(TAG, "Failure: %s", livekit_failure_reason_str(reason));
        }
    }

    if (on_state_changed_) {
        on_state_changed_(mapped);
    }
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
    livekit_room_data_stream_topic_register(room_handle_, "transcription", &handler);
}

esp_err_t LiveKitSession::Connect(const Esp32HubConfig& config)
{
    if (room_handle_ != nullptr) {
        Disconnect();
    }

    esp_err_t media_err = eidolon_livekit_board_init();
    if (media_err != ESP_OK) {
        return media_err;
    }

    esp_capture_handle_t capturer = eidolon_livekit_board_get_capturer();
    av_render_handle_t renderer = eidolon_livekit_board_get_renderer();
    if (!capturer || !renderer) {
        ESP_LOGE(TAG, "Media pipeline not ready");
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
    room_options.ctx = this;

    if (livekit_room_create(&room_handle_, &room_options) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_create failed");
        room_handle_ = nullptr;
        return ESP_FAIL;
    }

    RegisterTranscriptionHandler();

    ESP_LOGI(TAG, "Connecting room=%s identity=%s server=%s", config.room_name.c_str(),
             config.identity.c_str(), config.server_url.c_str());

    if (livekit_room_connect(room_handle_, config.server_url.c_str(), config.token.c_str()) !=
        LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_connect failed");
        livekit_room_destroy(room_handle_);
        room_handle_ = nullptr;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t LiveKitSession::Disconnect()
{
    connected_ = false;
    if (room_handle_ == nullptr) {
        return ESP_OK;
    }

    livekit_room_data_stream_topic_unregister(room_handle_, "transcription");

    if (livekit_room_close(room_handle_) != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "livekit_room_close failed");
    }
    if (livekit_room_destroy(room_handle_) != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "livekit_room_destroy failed");
    }
    room_handle_ = nullptr;
    eidolon_livekit_board_deinit();
    return ESP_OK;
}

bool LiveKitSession::IsConnected() const
{
    return connected_;
}

}  // namespace eidolon
