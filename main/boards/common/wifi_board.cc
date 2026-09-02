#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#if CONFIG_EIDOLON_HUB_MODE
#include "eidolon/commissioning_runtime.h"
#include "eidolon/commissioning_transaction.h"
#include "eidolon/device_physical_recovery.h"
#include "eidolon/eidolon_runtime_status.h"
#include "eidolon/hub_trust_store.h"
#include "eidolon/provisioning_window_policy_core.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
#if CONFIG_EIDOLON_HUB_MODE
    if (!eidolon::RecoverPendingCommissioningTransaction()) {
        ESP_LOGE(TAG, "Commissioning transaction recovery is still pending; refusing Station start");
        return;
    }
#endif
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "eidolon";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        OpenSetupWithoutAnyonePresent();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
#if CONFIG_EIDOLON_HUB_MODE
    // While commissioning owns the radio lease, legacy network callbacks are
    // observations only. In particular they cannot start activation or replace
    // the actor's confirmed UI projection. The one restored Station route is
    // transferred back through the actor before Application sees it.
    bool commissioning_owns_event =
        eidolon::CommissioningRuntime::GetInstance().IsInProgress();
    if (event == NetworkEvent::Connected && commissioning_owns_event) {
        commissioning_owns_event =
            eidolon::CommissioningRuntime::GetInstance()
                .NotifyStationRouteReady() || commissioning_owns_event;
    }
#endif
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_
#if CONFIG_EIDOLON_HUB_MODE
        && !commissioning_owns_event
#endif
    ) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout");

#if CONFIG_EIDOLON_HUB_MODE
    // The timeout expresses intent only. StopStation belongs to the
    // commissioning actor's AcquireCommissioningRadioLease action — and on a
    // commissioned device the request below is refused, so Station is left
    // running and keeps scanning with backoff, which is how the network is
    // allowed to come back on its own.
    board->OpenSetupWithoutAnyonePresent();
#else
    WifiManager::GetInstance().StopStation();
    board->OpenSetupWithoutAnyonePresent();
#endif
}

void WifiBoard::OpenSetupWithoutAnyonePresent() {
#if CONFIG_EIDOLON_HUB_MODE
    // The second half of the D8 rule, and the half the removal fix deliberately
    // left open:
    //
    //   A device that already has an Owner may not open a setup window because
    //   its network went away. That is a network fault, and a network fault
    //   projects recovery_required — it never widens the takeover surface.
    //
    // Forbidden path D8 (docs/设备与Body/设备生命周期状态机与恢复边.md, line 110):
    // 连不上网络后自动开设置窗口 → 只进入 NetworkRecoveryRequired；物理在场或已认证
    // 管理员才能开有界窗口. §6.3 and §6.4 repeat it for the network and
    // commissioning planes: 网络故障只产生 recovery_required，不会自动进入 open.
    //
    // Without this, a router that rebooted, a Wi-Fi password somebody changed,
    // or anyone able to take the network away for sixty seconds made a
    // commissioned device advertise itself for setup with nobody in the room.
    // The window was bounded, which is not the point: a bounded offer is still
    // an offer, and the attacker chooses when it opens.
    //
    // The trust store decides it, read through the same function as the
    // window-bounding decision — a device cannot be commissioned enough to get
    // a bounded window and uncommissioned enough to open one for itself. A
    // device with no commissioned Owner Domain keeps the old behaviour, because
    // that is the out-of-the-box path and refusing there would leave a board
    // that cannot be set up at all.
    //
    // Refusing is not giving up. Station keeps scanning on its own backoff, so
    // §6.3's other exit edge is live — 网络自行恢复后由设备证据回到 connected — and
    // the phase projected here says exactly that: still connecting. Not
    // RecoveryRequired, which in this firmware means a person must act before
    // anything can change, and is the true thing to say about a removal but a
    // false thing to say about a router that will be back in a minute. The way
    // in for a network that really did change is the gesture, which the detail
    // names because nothing else on screen does.
    if (eidolon::AutomaticSetupOpenIsForbidden(
            eidolon::ProvisioningWindowTriggerFor(
                eidolon::OwnerTrustStore().CommissionedOwnerDomainId()))) {
        ESP_LOGW(TAG,
                 "Refusing to open setup: this device has an Owner and cannot "
                 "reach its network; network_recovery_required (D8). Still "
                 "retrying Wi-Fi; long-press to set up a different network");
        Application::GetInstance().SetEidolonRuntimeUi(
            eidolon::RuntimePhase::NetworkConnecting,
            "Cannot reach your Wi-Fi. Still trying - press and hold the button "
            "to set up a different network");
        return;
    }
#endif
    StartWifiConfigMode();
}


void WifiBoard::StartWifiConfigMode() {
#if CONFIG_EIDOLON_HUB_MODE
    // The rule, stated once, where every request for a window passes:
    //
    //   A device may open a commissioning window only when it is commissionable,
    //   and the RemovalJournal is the fact that decides that — consulted here,
    //   before the window opens, not later by the Claim that fails.
    //
    // The two automatic callers — a boot with no network profile, and a connect
    // timeout — now arrive through OpenSetupWithoutAnyonePresent, which turns
    // them away on a device that has an Owner (forbidden path D8). They still
    // reach this line on a device that has none, and that is the case this
    // check exists for: an Owner erase takes the network with it, so after a
    // remote erase both of them fire, and the window-bounding decision
    // downstream reads an empty trust store and calls this a device with
    // nothing to give away. It is not — the same erase left a RemovalJournal
    // that refuses every Claim, so the window it opened could only ever end in
    // one: an operator handed their Wi-Fi and their Host to a device that could
    // not register, and a Controller stalled on the last step.
    //
    // §1 item 10: rejoining is not re-provisioning. Only physical presence or an
    // authenticated admin may open a window, and physical presence arrives here
    // through EnterWifiConfigMode, which consumes the removal terminal first —
    // so by the time it reaches this line the journal no longer blocks and one
    // check covers both doors.
    //
    // Deliberately not the other repair: advertising anyway with "physical
    // recovery required" written into the setup descriptor, so the Controller
    // could explain the dead end. That is a better dead end and still a window
    // open with nobody present, which is the thing D8 forbids. What explains
    // the dead end instead is the device's own screen, set here, and a phone
    // that simply does not find a device it must not be offered.
    if (eidolon::DevicePhysicalRecovery::RemovalBlocksCommissioning()) {
        ESP_LOGE(TAG,
                 "Refusing to open setup: a removal on record is still terminal. "
                 "Long-press BOOT to rejoin");
        Application::GetInstance().SetEidolonRuntimeUi(
            eidolon::RuntimePhase::RecoveryRequired,
            "Removed from this Owner. Press and hold the button to claim it again");
        return;
    }
    // This is an intent boundary. Identity, radio, transport, persistence and
    // user-visible state are all owned by the commissioning actor.
    if (!eidolon::CommissioningRuntime::GetInstance().RequestOpen()) {
        ESP_LOGE(TAG, "Commissioning runtime did not accept the request");
        return;
    }
    // A manual setup request must revoke the legacy boot timeout immediately;
    // otherwise it can fire in the middle of the actor-owned generation and
    // become a second writer of the Wi-Fi driver.
    esp_timer_stop(connect_timer_);
#elif defined(CONFIG_USE_HOTSPOT_WIFI_PROVISIONING)
    in_config_mode_ = true;
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

#if CONFIG_EIDOLON_HUB_MODE
    // This entry point is the physical-presence boundary. Automated no-profile
    // setup and connect timeouts call StartWifiConfigMode directly and cannot
    // consume a signed removal terminal.
    if (!eidolon::DevicePhysicalRecovery::AuthorizeFromPhysicalPresence()) {
        ESP_LOGE(TAG, "Physical recovery transaction did not converge");
        return;
    }

    // In HUB_MODE every permitted state does the same thing — hand the act to
    // the commissioning actor — so one rule decides it, and that rule is pinned
    // by tests rather than spelled out again here.
    if (!eidolon::HubDeviceStateAllowsSetupOpen(state)) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called in device state %d, which cannot open setup", state);
        return;
    }
    StartWifiConfigMode();
    return;
#else
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting && state != kDeviceStateActivating) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called in device state %d, which cannot open setup", state);
        return;
    }

    StartWifiConfigMode();
#endif
}

bool WifiBoard::IsInWifiConfigMode() const {
#if CONFIG_EIDOLON_HUB_MODE
    return eidolon::CommissioningRuntime::GetInstance().IsAdvertising();
#else
    return WifiManager::GetInstance().IsConfigMode();
#endif
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
