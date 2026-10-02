#include "device_capabilities.h"
#include "livekit_session.h"
#include "session_playback_state.h"

#include "eidolon_topics.h"
#include "internal_memory_report.h"
#include "livekit_board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
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

AgentPhase PhaseFromUiState(const char* value, const char* reason)
{
    if (value && strcmp(value, "waiting") == 0) {
        if (reason && (strcmp(reason, "team:error") == 0 ||
                       strcmp(reason, "team:abstained") == 0)) {
            return AgentPhase::ResponseUnavailable;
        }
        if (reason && strcmp(reason, "team:budget_exhausted") == 0) {
            return AgentPhase::ReplyLimitReached;
        }
        return AgentPhase::AwaitingInput;
    }
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

void LiveKitSession::OnTextStreamOpen(const livekit_data_stream_header_t* header, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !header) return;
    session->transcription_stream_.Clear();
    session->transcription_source_ = TranscriptionSource::Unknown;
    if (!header->is_text || !CurrentOutputGate().Allows(presentation::Output::DialogueText)) return;
    if (header->sender_identity && session->identity_ == header->sender_identity) {
        session->transcription_source_ = TranscriptionSource::User;
    } else if (session->IsAgent(header->sender_identity)) {
        session->transcription_source_ = TranscriptionSource::Agent;
    } else {
        return;
    }
    session->transcription_stream_.Open(header->stream_id);
    if (session->transcription_source_ == TranscriptionSource::Agent)
        ESP_LOGI(TAG, "Assistant caption stream opened (incremental)");
}

void LiveKitSession::OnTextStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !chunk) return;
    if (!CurrentOutputGate().Allows(presentation::Output::DialogueText)) {
        session->transcription_stream_.Clear();
        return;
    }
    if (chunk->chunk_index == 0 && session->transcription_source_ == TranscriptionSource::Agent)
        ESP_LOGI(TAG, "Assistant caption first chunk (%u bytes)", static_cast<unsigned>(chunk->content_size));
    session->transcription_stream_.Append(chunk->stream_id, chunk->chunk_index,
                                          chunk->content, chunk->content_size);
    // LiveKit paces these deltas against speech. Forward a valid UTF-8 snapshot
    // now; waiting for the trailer discards all of that synchronization.
    if (session->transcription_source_ == TranscriptionSource::Agent && session->on_transcription_) {
        auto text = session->transcription_stream_.Snapshot(chunk->stream_id);
        if (!text.empty()) session->on_transcription_({TranscriptionSource::Agent, std::move(text), false});
    }
}

void LiveKitSession::OnTextStreamClose(const livekit_data_stream_trailer_t* trailer, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !trailer) return;
    auto text = session->transcription_stream_.Close(trailer->stream_id,
        (!trailer->reason || !*trailer->reason) &&
        CurrentOutputGate().Allows(presentation::Output::DialogueText));
    if (!text.empty() && session->on_transcription_) {
        // The ESP SDK does not expose transcription_final attributes. Do not
        // present interim user recognition as a final user utterance.
        const bool final = session->transcription_source_ == TranscriptionSource::Agent;
        session->on_transcription_({session->transcription_source_, text, final});
        ESP_LOGI(TAG, "Transcription stream closed (%u bytes)", static_cast<unsigned>(text.size()));
    }
}

void LiveKitSession::OnDrainStreamChunk(const livekit_data_stream_chunk_t* chunk, void* ctx)
{
    (void)chunk;
    (void)ctx;
}

// Participant kind comes from authenticated server signalling, never packet JSON.
void LiveKitSession::OnParticipantInfo(const livekit_participant_info_t* info, void* ctx)
{
    auto* session=static_cast<LiveKitSession*>(ctx);
    if (!session || !info || !info->identity || std::strlen(info->identity)>128) return;
    std::string pending;
    uint32_t generation;
    bool configuration_hint = false;
    {
        std::lock_guard<std::mutex> lock(session->peers_mutex_);
        generation=session->generation_;
        // The official callback delivers local ParticipantUpdate too. It does
        // not expose permissions, so this is only a hint to compare fresh
        // authenticated Owner configuration, never a reason to reopen media.
        configuration_hint = info->state == LIVEKIT_PARTICIPANT_STATE_ACTIVE &&
            !session->identity_.empty() && session->identity_ == info->identity;
        for (auto& peer:session->agent_peers_) {
            if (peer==info->identity) { peer.clear(); break; }
        }
        bool registered=false;
        if (info->kind==LIVEKIT_PARTICIPANT_KIND_AGENT &&
            info->state!=LIVEKIT_PARTICIPANT_STATE_DISCONNECTED) {
            for (auto& peer:session->agent_peers_) if (peer.empty()) {
                peer=info->identity;registered=true;break;
            }
        }
        pending=session->pending_session_control_.Resolve(
            info->identity,registered,generation,esp_timer_get_time()/1000);
    }
    if (configuration_hint && session->on_configuration_invalidated_) {
        session->on_configuration_invalidated_(generation);
    }
    if (!pending.empty() && session->on_session_control_) {
        ESP_LOGI(TAG,"Delivering session control after Agent signalling confirmation");
        session->on_session_control_(pending,generation,true);
    }
}
bool LiveKitSession::IsAgent(const char* identity)
{
    if (!identity || !identity[0]) return false;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto& peer:agent_peers_) if (peer==identity) return true;
    return false;
}

void LiveKitSession::OnDataReceived(const livekit_data_received_t* data, void* ctx)
{
    auto* session = static_cast<LiveKitSession*>(ctx);
    if (!session || !data || !data->payload.bytes || data->payload.size == 0) {
        return;
    }
    const char* topic = data->topic ? data->topic : "";
    if (strcmp(topic, kUiStateTopic) == 0) {
        if (kOutputPolicyRequired && !session->IsAgent(data->sender_identity)) return;
        session->HandleUiStatePayload(reinterpret_cast<const char*>(data->payload.bytes),
                                      data->payload.size);
        return;
    }
    if (strcmp(topic, kSessionControlTopic) == 0 && session->on_session_control_) {
        if (data->payload.size>1024) return;
        std::string payload(reinterpret_cast<const char*>(data->payload.bytes),
                            data->payload.size);
        bool agent=false;
        bool provider=false;
        uint32_t generation;
        {
            std::lock_guard<std::mutex> lock(session->peers_mutex_);
            generation=session->generation_;
            if (data->sender_identity) {
                provider = !session->provider_identity_.empty() &&
                           session->provider_identity_ == data->sender_identity;
                for (const auto& peer:session->agent_peers_)
                    if (!peer.empty() && peer==data->sender_identity) { agent=true;break; }
                if (!agent && !provider) session->pending_session_control_.Stage(
                    data->sender_identity,payload,generation,esp_timer_get_time()/1000);
            }
        }
        // Provider can reject a pending request, but never authorize a start/end.
        // Identity comes from LiveKit's authenticated transport, not packet JSON.
        if (provider) {
            cJSON* root = cJSON_Parse(payload.c_str());
            const char* type = root ? JsonString(root, "type") : nullptr;
            const bool rejection = type && strcmp(type, kSessionRejectedType) == 0;
            cJSON_Delete(root);
            if (rejection) session->on_session_control_(payload, generation, false);
            return;
        }
        if (!agent) {
            ESP_LOGI(TAG,"Session control awaits authenticated Agent signalling");
            return;
        }
        ESP_LOGI(TAG, "[lifecycle] session_control received topic=%s bytes=%u",
                 topic, static_cast<unsigned>(data->payload.size));
        session->on_session_control_(payload, generation, true);
        return;
    }
    if (strcmp(topic, kEventTopic) == 0 && session->on_device_event_) {
        std::string payload(reinterpret_cast<const char*>(data->payload.bytes),
                            data->payload.size);
        session->on_device_event_(payload, session->generation_);
        return;
    }
    if (strcmp(topic, kControlTopic) != 0 || !session->on_control_command_) {
        return;
    }
    bool provider = false;
    {
        std::lock_guard<std::mutex> lock(session->peers_mutex_);
        provider = data->sender_identity && !session->provider_identity_.empty() &&
                   session->provider_identity_ == data->sender_identity;
    }
    // Larger temporary bindings are accepted only from the authenticated Provider.
    if (data->payload.size > (provider ? 160 * 1024U : 4096U)) {
        ESP_LOGW(TAG, "Control packet rejected: bytes=%u provider=%d",
                 static_cast<unsigned>(data->payload.size), provider);
        return;
    }
    ESP_LOGI(TAG, "Control packet received: bytes=%u provider=%d gen=%lu",
             static_cast<unsigned>(data->payload.size), provider,
             static_cast<unsigned long>(session->generation_));
    std::string payload(reinterpret_cast<const char*>(data->payload.bytes), data->payload.size);
    session->on_control_command_(payload, session->generation_, session->IsAgent(data->sender_identity), provider);
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
    if (!payload || size == 0) {
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
        const char* reason = JsonString(root, "reason");
        const auto playback = SessionPlaybackSignal(state, reason);
        if (playback.has_value() && on_agent_playback_) {
            on_agent_playback_(*playback, generation_);
        }
        // Session activity is true even when this device is input-only.
        auto phase = PhaseFromUiState(state, reason);
        if ((phase == AgentPhase::UserSpeaking || phase == AgentPhase::AgentThinking) &&
            transcription_source_ == TranscriptionSource::Agent) {
            transcription_stream_.Clear(); // Discard late chunks from the interrupted turn.
        }
        if (on_agent_phase_) on_agent_phase_(phase);
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
        .on_open = OnTextStreamOpen,
        .on_close = OnTextStreamClose,
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

esp_err_t LiveKitSession::EnsureMediaBoard(bool audio_input, bool audio_output)
{
    if (media_board_initialized_) {
        return ESP_OK;
    }
    esp_err_t err = eidolon_livekit_board_init(audio_input, audio_output);
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

bool LiveKitSession::AdmitEngineMemory()
{
    const InternalHeapSnapshot heap = CaptureInternalHeap();
    const EngineInternalMemoryRequirement requirement =
        LiveKitEngineInternalMemoryRequirement();
    const SessionMemoryVerdict verdict = JudgeSessionMemory(heap, requirement);

    ESP_LOGI(TAG,
             "[mem] room_create verdict=%s internal free=%u largest_block=%u "
             "free_blocks=%u need_contiguous=%u need_total=%u",
             SessionMemoryVerdictName(verdict),
             static_cast<unsigned>(heap.free_bytes),
             static_cast<unsigned>(heap.largest_free_block),
             static_cast<unsigned>(heap.free_blocks),
             static_cast<unsigned>(LargestContiguousRequirement(requirement)),
             static_cast<unsigned>(TotalRequirement(requirement)));

    if (verdict == SessionMemoryVerdict::Sufficient) {
        return true;
    }

    const bool retry_worthwhile = memory_ledger_.RecordRefusal(heap, requirement);

    // One line that carries the whole failure: what is missing, how much of it,
    // and whether waiting can produce it. The predecessor of this message said
    // "Failed to create engine" and left the numbers on a different line with no
    // statement about either.
    ESP_LOGE(TAG,
             "Cannot build a LiveKit room: internal RAM %s. Need %u bytes in one "
             "block (short by %u) and %u bytes total (short by %u); have %u free in "
             "%u blocks, largest %u. Attempt %d, best block seen %u. Retrying %s.",
             SessionMemoryVerdictName(verdict),
             static_cast<unsigned>(LargestContiguousRequirement(requirement)),
             static_cast<unsigned>(ContiguousShortfallBytes(heap, requirement)),
             static_cast<unsigned>(TotalRequirement(requirement)),
             static_cast<unsigned>(TotalShortfallBytes(heap, requirement)),
             static_cast<unsigned>(heap.free_bytes),
             static_cast<unsigned>(heap.free_blocks),
             static_cast<unsigned>(heap.largest_free_block),
             memory_ledger_.consecutive_refusals(),
             static_cast<unsigned>(memory_ledger_.best_largest_free_block()),
             retry_worthwhile ? "may still help" : "will not help until something frees internal RAM");

    // Who is holding it. Printed only on refusal, because it is a dozen lines
    // and it is worth every one of them exactly here.
    LogInternalMemoryLedger("room_create_refused");
    return false;
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
        Disconnect();
    }

    transcription_stream_.Clear();
    identity_ = config.session.identity;
    provider_identity_ = config.session.room_name.empty()
        ? "" : std::string(kChannelProviderIdentityPrefix) + config.session.room_name;
    generation_ = generation;

    const auto local_audio = CompiledDeviceCapabilities().OutputMask() & kAudioOutputs;
    const bool audio_output = config.output_policy.known
        ? bool(config.output_policy.allowed & local_audio)
        : (!kOutputPolicyRequired && local_audio != 0);
    const bool audio_input = !config.output_policy.inputs_known || config.output_policy.microphone;
    esp_err_t media_err = EnsureMediaBoard(audio_input, audio_output);
    if (media_err != ESP_OK) {
        return media_err;
    }

    esp_capture_handle_t capturer = eidolon_livekit_board_get_capturer();
    av_render_handle_t renderer = eidolon_livekit_board_get_renderer();
    if ((audio_input && !capturer) || (audio_output && !renderer)) {
        ESP_LOGE(TAG, "Media pipeline not ready");
        ReleaseMediaBoard();
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t sample_rate = config.sample_rate > 0 ? config.sample_rate : 16000;
    uint8_t channels = config.channels > 0 ? config.channels : 1;

    livekit_room_options_t room_options = {};
    room_options.publish = {
        .kind = audio_input ? LIVEKIT_MEDIA_TYPE_AUDIO : LIVEKIT_MEDIA_TYPE_NONE,
        .audio_encode =
            {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = sample_rate,
                .channel_count = channels,
            },
        .capturer = capturer,
    };
    room_options.subscribe = {
        .kind = audio_output ? LIVEKIT_MEDIA_TYPE_AUDIO : LIVEKIT_MEDIA_TYPE_NONE,
        .renderer = renderer,
    };
    room_options.on_state_changed = OnRoomStateChanged;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& peer:agent_peers_) peer.clear();
        pending_session_control_.Clear();
    }
    room_options.on_data_received = OnDataReceived;
    room_options.on_participant_info = OnParticipantInfo;
    room_options.ctx = this;

    // The engine is built out of FreeRTOS objects, and every FreeRTOS object
    // comes from pvPortMalloc(), which ESP-IDF hardcodes to MALLOC_CAP_INTERNAL.
    // So the room's cost is paid entirely out of the one heap that the board's
    // 5.8 MB of PSRAM cannot relieve, and it is paid as two separate contiguous
    // blocks. Judge that before the call: livekit_room_create() answers a
    // starved heap with a bare "Failed to create engine", and one of its cleanup
    // paths frees the room while leaking the engine behind it.
    if (!AdmitEngineMemory()) {
        ReleaseMediaBoard();
        return ESP_ERR_NO_MEM;
    }

    if (livekit_room_create(&room_handle_, &room_options) != LIVEKIT_ERR_NONE) {
        // Admission said yes and the SDK still could not build it, so the
        // requirement model in session_memory_admission_core is short of what
        // this SDK pin actually asks for. Say that, rather than let it read as a
        // second unexplained memory failure, and record the attempt so a run of
        // them still terminates.
        const InternalHeapSnapshot heap = CaptureInternalHeap();
        const EngineInternalMemoryRequirement requirement =
            LiveKitEngineInternalMemoryRequirement();
        ESP_LOGE(TAG,
                 "livekit_room_create failed despite passing admission "
                 "(internal free=%u largest_block=%u free_blocks=%u, modelled need "
                 "total=%u contiguous=%u) — the requirement model is under-counting",
                 static_cast<unsigned>(heap.free_bytes),
                 static_cast<unsigned>(heap.largest_free_block),
                 static_cast<unsigned>(heap.free_blocks),
                 static_cast<unsigned>(TotalRequirement(requirement)),
                 static_cast<unsigned>(LargestContiguousRequirement(requirement)));
        LogInternalMemoryLedger("room_create_failed");
        memory_ledger_.RecordRefusal(heap, requirement);
        room_handle_ = nullptr;
        transcription_registered_ = false;
        agent_session_registered_ = false;
        // Release the media board on the way out. Leaving the codec/AFE/renderer
        // resident makes the caller's bounded retry fail for the very same reason
        // this attempt did, turning one recoverable failure into a flap.
        ReleaseMediaBoard();
        return ESP_ERR_NO_MEM;
    }
    memory_ledger_.Reset();

    RegisterTranscriptionHandler();
    RegisterAgentSessionDrainHandler();
    using_media_ = true;

    ESP_LOGI(TAG, "Connecting room=%s identity=%s server=%s", config.session.room_name.c_str(),
             config.session.identity.c_str(), config.session.server_url.c_str());

    if (livekit_room_connect(room_handle_, config.session.server_url.c_str(),
                             config.session.token.c_str()) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_room_connect failed");
        UnregisterStreamHandlers();
        livekit_room_destroy(room_handle_);
        room_handle_ = nullptr;
        using_media_ = false;
        ReleaseMediaBoard();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t LiveKitSession::Disconnect()
{
    connected_ = false;
    if (room_handle_ == nullptr) {
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (auto& peer:agent_peers_) peer.clear();
            pending_session_control_.Clear();
        }
        identity_.clear();
        provider_identity_.clear();
        last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
        if (media_board_initialized_) {
            ESP_LOGI(TAG, "Releasing LiveKit media board without active room");
            ReleaseMediaBoard();
            using_media_ = false;
        }
        return ESP_OK;
    }

    livekit_room_handle_t handle = room_handle_;
    ESP_LOGI(TAG, "Disconnecting room");

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
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& peer:agent_peers_) peer.clear();
        pending_session_control_.Clear();
    }
    room_handle_ = nullptr;
    transcription_stream_.Clear();
    identity_.clear();
    provider_identity_.clear();
    last_failure_reason_ = LIVEKIT_FAILURE_REASON_NONE;
    if (media_board_initialized_) {
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
    const int64_t started_us = esp_timer_get_time();
    const auto result = livekit_room_publish_data(room_handle_, &options);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us >= 80000) {
        ESP_LOGW(TAG, "[publish] topic=%s reliable=%d elapsed_ms=%lld result=%d",
                 topic.c_str(), reliable, elapsed_us / 1000, static_cast<int>(result));
    }
    if (result != LIVEKIT_ERR_NONE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace eidolon
