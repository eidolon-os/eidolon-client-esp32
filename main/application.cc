#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "eidolon/eidolon_build_stamp.h"
#include "eidolon/eidolon_device_profile.h"

#if CONFIG_EIDOLON_HUB_MODE
#include "eidolon/eidolon_audio_input.h"
#include "eidolon/commissioning_runtime.h"
#include "eidolon/eidolon_device_store.h"
#include "eidolon/eidolon_ui_presenter.h"
#if CONFIG_EIDOLON_GUARD_SERVICE
#include "eidolon/guard/guard_service.h"
#endif
#include "eidolon/hub_activator.h"
#include "eidolon/hub_types.h"
#include "eidolon/livekit_voice_transport.h"
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#if CONFIG_EIDOLON_WAKE_WORD_ENABLE
#include "eidolon/audio/eidolon_audio_input_service.h"
#endif
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
#if CONFIG_EIDOLON_HUB_MODE
#if CONFIG_EIDOLON_WAKE_WORD_ENABLE
    if (eidolon_audio_input_service_) {
        eidolon_audio_input_service_->Stop();
        eidolon_audio_input_service_.reset();
    }
#endif
    voice_transport_.reset();
#if CONFIG_EIDOLON_GUARD_SERVICE
    guard_service_.reset();
#endif
    ui_presenter_.reset();
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

#if CONFIG_EIDOLON_HUB_MODE
bool Application::IsVoiceDetected() const
{
    return false;
}

bool Application::IsMicrophoneEnabled() const
{
    return voice_transport_ && voice_transport_->IsMicrophoneEnabled();
}

eidolon::GuardService* Application::GetGuardService()
{
#if CONFIG_EIDOLON_GUARD_SERVICE
    return guard_service_.get();
#else
    return nullptr;
#endif
}

void Application::SetEidolonLifecycleUi(eidolon::LifecyclePhase phase,
                                        const std::string& detail)
{
    if (ui_presenter_) {
        ui_presenter_->SetLifecyclePhase(phase, detail);
    }
}

void Application::RequestVoiceJoin()
{
    ESP_LOGI(TAG, "[voice_request] RequestVoiceJoin transport=%d state=%s",
             voice_transport_ ? 1 : 0,
             voice_transport_
                 ? eidolon::EidolonVoiceController::VoiceStateName(
                       voice_transport_->GetSessionState())
                 : "none");
    if (voice_transport_) {
        voice_transport_->JoinSession();
    }
}

void Application::RequestVoiceLeave()
{
    ESP_LOGI(TAG, "[voice_request] RequestVoiceLeave transport=%d state=%s",
             voice_transport_ ? 1 : 0,
             voice_transport_
                 ? eidolon::EidolonVoiceController::VoiceStateName(
                       voice_transport_->GetSessionState())
                 : "none");
    if (voice_transport_) {
        voice_transport_->LeaveSession();
    }
}

void Application::OnAmbientPresenceChanged(bool present)
{
    if (voice_transport_) {
        voice_transport_->OnAmbientPresenceChanged(present);
    }
}

void Application::ToggleVoiceSession()
{
    Schedule([this]() {
        ESP_LOGI(TAG, "[voice_request] ToggleVoiceSession transport=%d state=%s",
                 voice_transport_ ? 1 : 0,
                 voice_transport_
                     ? eidolon::EidolonVoiceController::VoiceStateName(
                           voice_transport_->GetSessionState())
                     : "none");
        if (voice_transport_) {
            voice_transport_->ToggleSession();
        }
    });
}

void Application::ToggleMicrophone()
{
    Schedule([this]() {
        if (!voice_transport_) {
            return;
        }
        if (!voice_transport_->IsInSession()) {
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification(Lang::Strings::ROOM_NOT_ACTIVE);
            return;
        }
        voice_transport_->SetMicrophoneEnabled(!voice_transport_->IsMicrophoneEnabled());
        if (ui_presenter_) {
            ui_presenter_->Apply(voice_transport_->GetSessionState(),
                                 voice_transport_->IsMicrophoneEnabled(),
                                 voice_transport_->LastEndReason());
        }
    });
}

void Application::PttPress()
{
    Schedule([this]() {
        if (!voice_transport_) {
            ESP_LOGW(TAG, "[voice_request] PttPress ignored transport=none");
            return;
        }
        // Hold-to-talk only applies once in the room. While not connected the talk
        // button acts as a "connect" button (tap) — ignore the hold so we don't
        // flash recording UI or drop a half-captured first utterance.
        if (!voice_transport_->IsInSession()) {
            ESP_LOGI(TAG, "[voice_request] PttPress ignored state=%s in_session=0",
                     eidolon::EidolonVoiceController::VoiceStateName(
                         voice_transport_->GetSessionState()));
            return;
        }
        ESP_LOGI(TAG, "[voice_request] PttPress accepted state=%s",
                 eidolon::EidolonVoiceController::VoiceStateName(
                     voice_transport_->GetSessionState()));
        voice_transport_->PttPress();
        if (ui_presenter_) {
            ui_presenter_->SetPttRecording(true);
        }
    });
}

void Application::PttRelease()
{
    Schedule([this]() {
        if (!voice_transport_) {
            ESP_LOGW(TAG, "[voice_request] PttRelease ignored transport=none");
            return;
        }
        if (!voice_transport_->IsInSession()) {
            ESP_LOGI(TAG, "[voice_request] PttRelease ignored state=%s in_session=0",
                     eidolon::EidolonVoiceController::VoiceStateName(
                         voice_transport_->GetSessionState()));
            return;
        }
        ESP_LOGI(TAG, "[voice_request] PttRelease accepted state=%s",
                 eidolon::EidolonVoiceController::VoiceStateName(
                     voice_transport_->GetSessionState()));
        voice_transport_->PttRelease();
        if (ui_presenter_) {
            ui_presenter_->SetPttRecording(false);
        }
    });
}

#if CONFIG_EIDOLON_WAKE_WORD_ENABLE
void Application::StartEidolonWakeWord()
{
    if (eidolon::EidolonAudioInput::Instance().Init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Eidolon audio platform");
        return;
    }

    if (!eidolon_audio_input_service_) {
        eidolon_audio_input_service_ = std::make_unique<EidolonAudioInputService>();
        auto codec = Board::GetInstance().GetAudioCodec();
        eidolon_audio_input_service_->Initialize(codec);

        EidolonAudioInputCallbacks callbacks;
        callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
            (void)wake_word;
            Schedule([this]() {
                if (!voice_transport_) {
                    return;
                }
                if (voice_transport_->GetSessionState() != eidolon::VoiceSessionState::ConfigReady) {
                    return;
                }
                ESP_LOGI(TAG, "Wake word -> ToggleVoiceSession");
                ToggleVoiceSession();
            });
        };
        eidolon_audio_input_service_->SetCallbacks(callbacks);
        eidolon_audio_input_service_->Start();
    }

    OnEidolonVoiceSessionState(voice_transport_ ? voice_transport_->GetSessionState()
                                                : eidolon::VoiceSessionState::Idle);
}

void Application::OnEidolonVoiceSessionState(eidolon::VoiceSessionState state)
{
    if (!eidolon_audio_input_service_) {
        return;
    }

    switch (state) {
    case eidolon::VoiceSessionState::Connecting:
    case eidolon::VoiceSessionState::InRoom:
    case eidolon::VoiceSessionState::Reconnecting:
    case eidolon::VoiceSessionState::PendingApproval:
    case eidolon::VoiceSessionState::WaitingBinding:
    case eidolon::VoiceSessionState::ConfigReady:
    case eidolon::VoiceSessionState::Idle:
    case eidolon::VoiceSessionState::Error:
    case eidolon::VoiceSessionState::Unauthorized:
    case eidolon::VoiceSessionState::ServerUnreachable:
        // Wake word is kept in the build but intentionally disabled for now.
        // During a voice session the codec input is owned by the LiveKit AFE
        // capture path (EidolonMicCapture); re-enabling wake word needs the
        // single-reader fan-out described in the AEC architecture doc.
        eidolon_audio_input_service_->EnableWakeWordDetection(false);
        break;
    }
}
#endif

void Application::ApplyEidolonDeviceUi(DeviceState state)
{
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    board.GetLed()->OnStateChanged();
    (void)state;
    display->UpdateStatusBar(true);
}
#else
bool Application::IsVoiceDetected() const
{
    return audio_service_.IsVoiceDetected();
}
#endif

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    // One-line build fingerprint (git commit / branch / LiveKit SDK), printed early
    // and unconditionally on every boot so we can confirm exactly which firmware +
    // SDK the device is running — PROJECT_VER alone is a static "1.0.0".
    // eidolon-common.sh reads this back over serial after flashing to catch a
    // stale binary / wrong flash path / cached SDK.
    ESP_LOGW(TAG, "EIDOLON-BUILDSTAMP git=%s sdk=%s branch=%s built=%s %s",
             EIDOLON_BUILD_GIT, EIDOLON_BUILD_SDK, EIDOLON_BUILD_BRANCH, __DATE__, __TIME__);
    // Resolved device profile on the line right after it: which interaction mode
    // and which capture topology this image actually ended up with.
    eidolon::LogDeviceProfile(TAG);

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();

#if CONFIG_EIDOLON_HUB_MODE
    // The UI presenter is created before assets/network so it is the sole owner
    // of every visible Eidolon lifecycle and voice state from this point on.
    ui_presenter_ = std::make_unique<eidolon::EidolonUiPresenter>(*this);
    SetEidolonLifecycleUi(eidolon::LifecyclePhase::LoadingAssets);
    ApplyLocalAssets();
    SetEidolonLifecycleUi(eidolon::LifecyclePhase::Booting);

    // Commissioning is a separate actor. Application consumes its confirmed
    // projection; it never infers setup progress from SoftAP callbacks or from
    // commands such as StartStation(). The committed handoff is the sole event
    // that may begin Hub enrollment after setup.
    eidolon::CommissioningRuntime::GetInstance().SetObserver(
        [this](const eidolon::CommissioningRuntimeSnapshot& snapshot) {
            Schedule([this, snapshot]() {
                using State = eidolon::CommissioningRuntimeState;
                switch (snapshot.state) {
                case State::PreparingIdentity:
                case State::AcquiringRadio:
                case State::StartingTransport:
                    SetEidolonLifecycleUi(
                        eidolon::LifecyclePhase::WifiConnecting,
                        "Preparing secure device setup...");
                    break;
                case State::Advertising:
                case State::SessionActive:
                    SetDeviceState(kDeviceStateWifiConfiguring);
                    SetEidolonLifecycleUi(
                        eidolon::LifecyclePhase::WifiSetup,
                        "Ready for secure device setup");
                    break;
                case State::ApplyingConfiguration:
                    SetEidolonLifecycleUi(
                        eidolon::LifecyclePhase::WifiConnecting,
                        snapshot.transaction_committed
                            ? "Wi-Fi and Host confirmed"
                            : "Validating Wi-Fi and Host...");
                    break;
                case State::ReturningToPreviousMode:
                case State::RestoringPreviousMode:
                    SetEidolonLifecycleUi(
                        eidolon::LifecyclePhase::WifiConnecting,
                        "Returning to the confirmed Wi-Fi route...");
                    break;
                case State::Idle:
                    if (snapshot.station_route_ready) {
                        SetEidolonLifecycleUi(
                            eidolon::LifecyclePhase::HubDiscovering,
                            "Wi-Fi route confirmed");
                        xEventGroupSetBits(event_group_,
                                           MAIN_EVENT_NETWORK_CONNECTED);
                    } else if (snapshot.previous_station_mode) {
                        SetEidolonLifecycleUi(
                            eidolon::LifecyclePhase::WifiScanning,
                            "Restoring the previous Wi-Fi route...");
                    } else {
                        SetEidolonLifecycleUi(
                            eidolon::LifecyclePhase::Offline,
                            "Secure setup window closed");
                    }
                    break;
                }
            });
        });

    {
        eidolon::EidolonDeviceStore device_store;
        if (!device_store.LoadThemeApplied()) {
            Settings display_settings("display", true);
            display_settings.SetString("theme", "eidolon_dark");
            device_store.MarkThemeApplied();
        }
    }

    eidolon::VoiceSessionCallbacks callbacks;
    callbacks.on_session_state = [this](eidolon::VoiceSessionState state) {
        ESP_LOGI(TAG, "[EIDOLON_UI] queue session_state=%s",
                 eidolon::EidolonVoiceController::VoiceStateName(state));
        Schedule([this, state]() {
            if (!voice_transport_ || !ui_presenter_) {
                return;
            }
            ESP_LOGI(TAG, "[EIDOLON_UI] dispatch session_state=%s",
                     eidolon::EidolonVoiceController::VoiceStateName(state));
            ui_presenter_->Apply(state, voice_transport_->IsMicrophoneEnabled(),
                                 voice_transport_->LastEndReason());
            if (network_connected_ && hub_activation_done_) {
                ui_presenter_->SetLifecyclePhase(eidolon::LifecyclePhase::Operational);
            }
#if CONFIG_EIDOLON_WAKE_WORD_ENABLE
            OnEidolonVoiceSessionState(state);
#endif
        });
    };
    callbacks.on_transcription = [this](const eidolon::TranscriptionEvent& event) {
        Schedule([this, event]() {
            if (ui_presenter_) {
                ui_presenter_->OnTranscription(event);
            }
        });
    };
    callbacks.on_agent_phase = [this](eidolon::AgentPhase phase) {
        Schedule([this, phase]() {
            if (ui_presenter_) {
                ui_presenter_->OnAgentPhase(phase);
            }
            // Motor-rail noise gate (StackChan): in half_duplex the mic is hot for
            // uplink in every phase except while the agent is speaking (the mic is
            // closed then). Cut the board's servo power rail whenever the mic is hot
            // so its switching whine can't rail the ADC and drown out near-end speech;
            // restore it when the agent speaks so the head can animate. No-op on boards
            // without a noisy motor rail.
            const bool mic_hot = phase != eidolon::AgentPhase::AgentSpeaking &&
                                 (!voice_transport_ || voice_transport_->IsMicrophoneEnabled());
            Board::GetInstance().SetCaptureQuiet(mic_hot);
        });
    };
    callbacks.on_presence_wake_phase = [this](eidolon::PresenceWakePhase phase) {
        Schedule([this, phase]() {
            if (ui_presenter_) {
                ui_presenter_->OnPresenceWakePhase(phase);
            }
        });
    };
    callbacks.on_ptt_turn_status = [this](const std::string& outcome) {
        Schedule([this, outcome]() {
            if (ui_presenter_) {
                ui_presenter_->OnPttTurnStatus(outcome);
            }
        });
    };
#if CONFIG_EIDOLON_GUARD_SERVICE
    guard_service_ = std::make_unique<eidolon::GuardService>(board.GetCamera());
    voice_transport_ = eidolon::CreateLiveKitVoiceTransport(std::move(callbacks), guard_service_.get());
#else
    voice_transport_ = eidolon::CreateLiveKitVoiceTransport(std::move(callbacks));
#endif
#else
    // Print board name/version info
    display->SetChatMessage("system", Lang::Strings::INITIALIZING);

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);
#endif

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
#if CONFIG_EIDOLON_HUB_MODE
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::WifiScanning);
#else
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
#if CONFIG_EIDOLON_HUB_MODE
                std::string detail = "Connecting to Wi-Fi";
                if (!data.empty()) {
                    detail += " ";
                    detail += data;
                }
                detail += "...";
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::WifiConnecting, detail);
#else
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
#endif
                break;
            }
            case NetworkEvent::Connected: {
#if CONFIG_EIDOLON_HUB_MODE
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::HubDiscovering,
                                      "Wi-Fi connected");
#else
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
#if CONFIG_EIDOLON_HUB_MODE
                if (!eidolon::CommissioningRuntime::GetInstance().IsInProgress()) {
                    SetEidolonLifecycleUi(eidolon::LifecyclePhase::Offline,
                                          "Wi-Fi disconnected");
                }
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
#if CONFIG_EIDOLON_HUB_MODE
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::WifiSetup);
#endif
                break;
            case NetworkEvent::WifiConfigModeExit:
#if CONFIG_EIDOLON_HUB_MODE
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::WifiScanning,
                                      "Applying Wi-Fi settings...");
#endif
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

#if !CONFIG_EIDOLON_HUB_MODE
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }
#endif

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();
#if CONFIG_EIDOLON_HUB_MODE
    network_connected_ = true;
#endif

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
#if CONFIG_EIDOLON_HUB_MODE
        SetEidolonLifecycleUi(eidolon::LifecyclePhase::HubDiscovering);
#endif
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    } else {
#if CONFIG_EIDOLON_HUB_MODE
        if (voice_transport_) {
            voice_transport_->OnNetworkRestored();
        }
        if (hub_activation_done_) {
            SetEidolonLifecycleUi(eidolon::LifecyclePhase::Operational);
        }
#endif
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
#if CONFIG_EIDOLON_HUB_MODE
    network_connected_ = false;
    auto state = GetDeviceState();
    if (state != kDeviceStateStarting &&
        state != kDeviceStateWifiConfiguring &&
        state != kDeviceStateActivating &&
        !eidolon::CommissioningRuntime::GetInstance().IsInProgress()) {
        SetEidolonLifecycleUi(eidolon::LifecyclePhase::Offline);
    }
    if (voice_transport_) {
        voice_transport_->OnNetworkLost();
    }
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_) {
        guard_service_->Stop("network_lost");
    }
#endif
#else
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }
#endif

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    auto& board = Board::GetInstance();

#if CONFIG_EIDOLON_HUB_MODE
    has_server_time_ = false;
    hub_activation_done_ = true;
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "[EIDOLON_UI] firmware ready version=%s", app_desc->version);
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (voice_transport_) {
        voice_transport_->OnActivationComplete();
        if (ui_presenter_) {
            ui_presenter_->Apply(voice_transport_->GetSessionState(),
                                 voice_transport_->IsMicrophoneEnabled(),
                                 voice_transport_->LastEndReason());
        }
    }
#if CONFIG_EIDOLON_WAKE_WORD_ENABLE
    StartEidolonWakeWord();
#endif
#else
    auto display = board.GetDisplay();
    has_server_time_ = ota_->HasServerTime();

    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    ota_.reset();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
#endif
}

void Application::ActivationTask() {
#if CONFIG_EIDOLON_HUB_MODE
    // HUB_MODE never runs the Xiaozhi version-check, so it never reaches the
    // mark-valid path inside CheckNewVersion(). Commit the running firmware here so an
    // anti-rollback reset (e.g. a user power-cycle) before hub activation completes
    // cannot abort the app and leave the device unbootable. We are already past board
    // bring-up and WiFi connect by the time activation runs.
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (running != nullptr &&
            esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Marked firmware valid (HUB_MODE boot commit)");
        }
    }

    CheckAssetsVersion();

    SetEidolonLifecycleUi(eidolon::LifecyclePhase::HubDiscovering);
    eidolon::HubActivator activator;
    if (!activator.Run()) {
        ESP_LOGE(TAG, "Hub activation failed, staying in activating state");
        activation_task_handle_ = nullptr;
        vTaskDelete(NULL);
        return;
    }
#else
    ota_ = std::make_unique<Ota>();

    CheckAssetsVersion();
    CheckNewVersion();
    InitializeProtocol();
#endif

    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::ApplyLocalAssets() {
    if (assets_applied_) {
        return;
    }
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
#if CONFIG_EIDOLON_HUB_MODE
    assets_applied_ = assets.Apply(false);
#else
    assets_applied_ = assets.Apply();
#endif
    ESP_LOGI(TAG, "[EIDOLON_UI] local assets applied=%d", assets_applied_ ? 1 : 0);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
#if !CONFIG_EIDOLON_HUB_MODE
    auto display = board.GetDisplay();
#endif
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

#if CONFIG_EIDOLON_HUB_MODE
        SetEidolonLifecycleUi(eidolon::LifecyclePhase::Updating,
                              "Downloading UI assets...");
#endif
#if !CONFIG_EIDOLON_HUB_MODE
        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS,
                 download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
#endif
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
#if !CONFIG_EIDOLON_HUB_MODE
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);
#endif

        assets_applied_ = false;
#if CONFIG_EIDOLON_HUB_MODE
        bool success = assets.Download(download_url, [this](int progress, size_t speed) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([this, message = std::string(buffer)]() {
                SetEidolonLifecycleUi(eidolon::LifecyclePhase::Updating, message);
            });
        });
#else
        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });
#endif

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
#if CONFIG_EIDOLON_HUB_MODE
            SetEidolonLifecycleUi(eidolon::LifecyclePhase::Error,
                                  "UI asset update failed");
#else
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
#endif
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    ApplyLocalAssets();
#if !CONFIG_EIDOLON_HUB_MODE
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
#endif
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
#if CONFIG_EIDOLON_HUB_MODE
        display->SetStatus(Lang::Strings::EIDOLON_READY);
#else
        display->SetStatus(Lang::Strings::STANDBY);
#endif
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
#if CONFIG_EIDOLON_HUB_MODE
    auto state = GetDeviceState();
    if (state == kDeviceStateStarting) {
        return;
    }
    ToggleVoiceSession();
#else
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
#endif
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

#if CONFIG_EIDOLON_HUB_MODE
    ApplyEidolonDeviceUi(new_state);
    return;
#endif

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
#if !CONFIG_EIDOLON_HUB_MODE
    auto display = board.GetDisplay();
#endif

    std::string upgrade_url = url;
#if !CONFIG_EIDOLON_HUB_MODE
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;
#endif

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

#if CONFIG_EIDOLON_HUB_MODE
    SetEidolonLifecycleUi(eidolon::LifecyclePhase::Updating,
                          version.empty() ? "Installing firmware..."
                                          : "Installing firmware " + version);
#else
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
#endif
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

#if !CONFIG_EIDOLON_HUB_MODE
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());
#endif

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

#if CONFIG_EIDOLON_HUB_MODE
    bool upgrade_success = Ota::Upgrade(upgrade_url, [this](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([this, message = std::string(buffer)]() {
            SetEidolonLifecycleUi(eidolon::LifecyclePhase::Updating, message);
        });
    });
#else
    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });
#endif

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
#if CONFIG_EIDOLON_HUB_MODE
        SetEidolonLifecycleUi(eidolon::LifecyclePhase::Error,
                              "Firmware update failed");
#else
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
#endif
        vTaskDelay(pdMS_TO_TICKS(3000));
#if CONFIG_EIDOLON_HUB_MODE
        SetEidolonLifecycleUi(network_connected_ && hub_activation_done_
                                  ? eidolon::LifecyclePhase::Operational
                                  : eidolon::LifecyclePhase::Offline);
#endif
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
#if CONFIG_EIDOLON_HUB_MODE
        SetEidolonLifecycleUi(eidolon::LifecyclePhase::Updating,
                              "Update complete. Restarting...");
#else
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
#endif
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
