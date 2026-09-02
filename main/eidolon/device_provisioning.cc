#include "device_provisioning.h"

#include "device_identity.h"
#include "device_provisioning_protocol.h"
#include "owner_trust_commissioning_worker.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <protocomm_security.h>
#if CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
#include <wifi_provisioning/scheme_ble.h>
#else
#include <wifi_provisioning/scheme_softap.h>
#endif

#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#define TAG "Provisioning"

namespace eidolon {

namespace {

// The two Eidolon endpoints. The Wi-Fi endpoints beside them are the SDK's own,
// and the controller walks all of them over one authenticated session.
constexpr const char* kDescriptorEndpoint = "eidolon-descriptor";
constexpr const char* kTrustEndpoint = "eidolon-trust";
constexpr const char* kStatusEndpoint = "eidolon-status";
constexpr const char* kTerminalAckEndpoint = "eidolon-terminal-ack";

// The leading bytes of the verifier that belongs to the shared development
// passphrase. A build that claims a manufacturer-bound identity while carrying
// this value would be telling controllers a per-device secret protects it when
// every development board shares the same one, so that combination is refused
// here rather than discovered in the field.
constexpr const char* kDevelopmentVerifierPrefix = "8aada0073ed32e1b00edd09c3c3371c3";

constexpr bool StartsWith(const char* text, const char* prefix)
{
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

constexpr size_t HexLength(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// A verifier for protocomm's SRP6a group is 384 bytes and the salt is 16, so
// anything else is a build that never received real values — a stale sdkconfig
// keeping an older default, or a truncated copy. Caught here because a device
// that boots with an unusable verifier looks fine until a controller tries to
// set it up and the handshake fails with nothing to point at.
static_assert(HexLength(CONFIG_EIDOLON_PROVISIONING_VERIFIER_HEX) == 768,
              "CONFIG_EIDOLON_PROVISIONING_VERIFIER_HEX must be a 384-byte SRP6a verifier as hex. "
              "Generate one with scripts/eidolon/generate_provisioning_verifier.py; if this build "
              "previously had a different value, the board script must set it on the existing "
              "sdkconfig too, because ESP-IDF does not re-apply changed Kconfig defaults.");
static_assert(HexLength(CONFIG_EIDOLON_PROVISIONING_SALT_HEX) == 32,
              "CONFIG_EIDOLON_PROVISIONING_SALT_HEX must be a 16-byte salt as hex.");

#ifdef CONFIG_EIDOLON_PROVISIONING_MANUFACTURER_BOUND
static_assert(!StartsWith(CONFIG_EIDOLON_PROVISIONING_VERIFIER_HEX, kDevelopmentVerifierPrefix),
              "A manufacturer-bound build must carry its own SRP6a verifier, not the shared "
              "development one. Regenerate it with "
              "scripts/eidolon/generate_provisioning_verifier.py.");
#endif

// Read the salt and verifier this build was given. Development and production
// differ in these values and in nothing else — the act, the endpoints and the
// order are identical — so there is one code path here and no build-time branch
// on which kind of device this is.
bool DecodeHex(const char* hex, std::vector<uint8_t>& out)
{
    out.clear();
    if (hex == nullptr) {
        return false;
    }
    const size_t length = std::strlen(hex);
    if (length == 0 || (length % 2) != 0) {
        return false;
    }
    out.reserve(length / 2);
    for (size_t i = 0; i < length; i += 2) {
        char byte[3] = {hex[i], hex[i + 1], '\0'};
        char* end = nullptr;
        const unsigned long value = std::strtoul(byte, &end, 16);
        if (end != byte + 2) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

// What a controller sees while scanning. Board-derived rather than Host-derived:
// a device that belongs to nobody has no Host to name, and the controller
// confirms identity from the descriptor rather than from this string.
std::string ServiceName()
{
    const std::string mac = SystemInfo::GetMacAddress();
    std::string tail;
    for (char c : mac) {
        if (c != ':' && c != '-') {
            tail.push_back(c);
        }
    }
    if (tail.size() > 6) {
        tail = tail.substr(tail.size() - 6);
    }
    return std::string("eidolon-") + tail;
}

// protocomm frees what a handler allocated, so answers are handed over as a
// malloc'd copy rather than as a pointer into anything this file owns.
esp_err_t Answer(const std::string& body, uint8_t** outbuf, ssize_t* outlen)
{
    if (body.empty()) {
        return ESP_ERR_NO_MEM;
    }
    auto* copy = static_cast<uint8_t*>(malloc(body.size()));
    if (copy == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(copy, body.data(), body.size());
    *outbuf = copy;
    *outlen = static_cast<ssize_t>(body.size());
    return ESP_OK;
}

}  // namespace

DeviceProvisioningService& DeviceProvisioningService::GetInstance()
{
    static DeviceProvisioningService instance;
    return instance;
}

esp_err_t DeviceProvisioningService::HandleDescriptor(uint32_t, const uint8_t*, ssize_t,
                                                     uint8_t** outbuf, ssize_t* outlen, void*)
{
    auto& self = GetInstance();
    device_foundation::v1::SetupDescriptor descriptor;
    auto& identity = DeviceIdentity::GetInstance();
    if (identity.EnsureKeypair() != ESP_OK || identity.DeviceInstanceId().empty()) {
        return ESP_FAIL;
    }
    descriptor.device_id = identity.DeviceInstanceId();
    descriptor.device_kind = BOARD_TYPE;
    descriptor.display_name = BOARD_NAME;
    descriptor.identity_fingerprint = DeviceIdentity::GetInstance().Fingerprint();
    descriptor.session_id = self.session_id_;
    descriptor.expires_in = AdvertisedWindowSeconds(self.window_);
    // This build carries a shared development secret unless it was given a
    // per-device one, and says so rather than letting the controller assume.
#ifdef CONFIG_EIDOLON_PROVISIONING_MANUFACTURER_BOUND
    descriptor.trust = device_foundation::v1::SetupDescriptorTrust::ManufacturerBound;
#else
    descriptor.trust = device_foundation::v1::SetupDescriptorTrust::DevelopmentTofu;
#endif
    return Answer(BuildSetupDescriptorJson(descriptor), outbuf, outlen);
}

esp_err_t DeviceProvisioningService::HandleTrust(uint32_t, const uint8_t* inbuf, ssize_t inlen,
                                                uint8_t** outbuf, ssize_t* outlen, void*)
{
    auto& self = GetInstance();
    std::string response;
    bool staged = false;
    const size_t payload_size =
        inlen > 0 ? static_cast<size_t>(inlen) : 0;
    const esp_err_t result = self.events_.stage_trust
        ? self.events_.stage_trust(self.transport_generation_, inbuf,
                                   payload_size, response, staged)
        : ESP_ERR_INVALID_STATE;
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Owner trust request completed with %s",
                 esp_err_to_name(result));
    }
    if (response.empty()) {
        response = BuildTrustRefusedJson("owner trust worker is unavailable");
    }
    if (staged) {
        ESP_LOGI(TAG, "Owner trust is durable staging, not active trust");
    }
    return Answer(response, outbuf, outlen);
}

esp_err_t DeviceProvisioningService::HandleStatus(
    uint32_t, const uint8_t*, ssize_t, uint8_t** outbuf, ssize_t* outlen, void*)
{
    auto& self = GetInstance();
    if (!self.events_.commissioning_status) return ESP_ERR_INVALID_STATE;
    return Answer(self.events_.commissioning_status(self.transport_generation_),
                  outbuf, outlen);
}

esp_err_t DeviceProvisioningService::HandleTerminalAck(
    uint32_t, const uint8_t* inbuf, ssize_t inlen,
    uint8_t** outbuf, ssize_t* outlen, void*)
{
    auto& self = GetInstance();
    if (inbuf == nullptr || inlen <= 0 || !self.events_.terminal_ack) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string payload(reinterpret_cast<const char*>(inbuf),
                              static_cast<size_t>(inlen));
    const bool accepted = self.events_.terminal_ack(
        self.transport_generation_, payload);
    return Answer(accepted ? "{\"acknowledged\":true}"
                           : "{\"acknowledged\":false}",
                  outbuf, outlen);
}

void DeviceProvisioningService::HandleProvisioningEvent(void*, const char* event_base, int32_t event_id,
                                                       void* event_data)
{
    auto& self = GetInstance();
    const uint32_t generation =
        self.transport_generation_.load(std::memory_order_acquire);
    if (generation == 0) return;
    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK &&
            self.events_.authenticated_session_started) {
            self.events_.authenticated_session_started(generation);
        }
        return;
    }
    switch (event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "Provisioning session open as %s", ServiceName().c_str());
        self.manager_started_.store(true, std::memory_order_release);
        self.MaybeReportReady();
        break;
    case WIFI_PROV_CRED_RECV: {
        // The vendor stack has applied this candidate only to the live Wi-Fi
        // driver. This callback copies bounded evidence and returns; only the
        // commissioning actor may decide whether it becomes durable.
        const auto* config = static_cast<const wifi_sta_config_t*>(event_data);
        if (config != nullptr && self.events_.network_candidate_received) {
            const std::string ssid(reinterpret_cast<const char*>(config->ssid),
                                   strnlen(reinterpret_cast<const char*>(config->ssid),
                                           sizeof(config->ssid)));
            const std::string password(reinterpret_cast<const char*>(config->password),
                                       strnlen(reinterpret_cast<const char*>(config->password),
                                               sizeof(config->password)));
            self.events_.network_candidate_received(
                generation, ssid, password);
        }
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        // Unlike the endpoint this replaces, a wrong password is reported to the
        // controller while it is still connected, before anything is torn down.
        ESP_LOGW(TAG, "Provisioned network did not come up; controller was told");
        if (self.events_.wifi_connection_failed) {
            self.events_.wifi_connection_failed(generation);
        }
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioned network came up");
        if (self.events_.wifi_connected) {
            self.events_.wifi_connected(generation);
        }
        break;
    case WIFI_PROV_END:
        self.manager_started_.store(false, std::memory_order_release);
        // This callback never destroys resources. An unsolicited SDK end is
        // evidence for the actor; an actor-initiated end is already converging
        // through CleanupTransport().
        if (!self.cleanup_in_progress_.load(std::memory_order_acquire) &&
            self.events_.transport_ended_unexpectedly) {
            self.events_.transport_ended_unexpectedly(generation);
        }
        break;
    default:
        break;
    }
}

esp_err_t DeviceProvisioningService::RegisterEventHandlers()
{
    const uint32_t generation =
        transport_generation_.load(std::memory_order_acquire);
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &HandleProvisioningEvent, nullptr,
        &provisioning_event_instance_);
    if (err != ESP_OK) return err;
    resources_.Own(generation, CommissioningTransportResource::EventHandlers);

    err = esp_event_handler_instance_register(
        PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
        &HandleProvisioningEvent, nullptr, &security_event_instance_);
    return err;
}

esp_err_t DeviceProvisioningService::StartOwnedHttpServer()
{
#if CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
    return ESP_OK;
#else
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers =
        CommissioningTransportEndpointBudget::kRequiredUriHandlers;
    config.max_open_sockets = 1;
    config.lru_purge_enable = true;
    const esp_err_t err = httpd_start(
        reinterpret_cast<httpd_handle_t*>(&httpd_handle_), &config);
    if (err != ESP_OK) return err;

    const uint32_t generation =
        transport_generation_.load(std::memory_order_acquire);
    resources_.Own(generation, CommissioningTransportResource::HttpServer);
    // ESP-IDF's API is typed as void*, but protocomm expects the stable address
    // of the handle and dereferences it while registering every endpoint.
    wifi_prov_scheme_softap_set_httpd_handle(&httpd_handle_);
    ESP_LOGI(TAG, "Owned commissioning HTTP server has %u URI slots",
             static_cast<unsigned>(config.max_uri_handlers));
    return ESP_OK;
#endif
}

void DeviceProvisioningService::MaybeReportReady()
{
    if (!manager_started_.load(std::memory_order_acquire) ||
        !endpoints_registered_.load(std::memory_order_acquire) ||
        cleanup_in_progress_.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!ready_reported_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const uint32_t generation =
        transport_generation_.load(std::memory_order_acquire);
    if (generation != 0 && events_.transport_ready) {
        events_.transport_ready(generation, ServiceName());
    }
}

esp_err_t DeviceProvisioningService::StartTransport()
{
    if (!DecodeHex(CONFIG_EIDOLON_PROVISIONING_SALT_HEX, salt_) ||
        !DecodeHex(CONFIG_EIDOLON_PROVISIONING_VERIFIER_HEX, verifier_)) {
        ESP_LOGE(TAG, "Provisioning salt/verifier are not usable hex");
        return ESP_ERR_INVALID_STATE;
    }

    security_params_ = {};
    security_params_.salt = reinterpret_cast<const char*>(salt_.data());
    security_params_.salt_len = static_cast<uint16_t>(salt_.size());
    security_params_.verifier = reinterpret_cast<const char*>(verifier_.data());
    security_params_.verifier_len = static_cast<uint16_t>(verifier_.size());

    if (wifi_prov_mgr_endpoint_create(kDescriptorEndpoint) != ESP_OK ||
        wifi_prov_mgr_endpoint_create(kTrustEndpoint) != ESP_OK ||
        wifi_prov_mgr_endpoint_create(kStatusEndpoint) != ESP_OK ||
        wifi_prov_mgr_endpoint_create(kTerminalAckEndpoint) != ESP_OK) {
        ESP_LOGE(TAG, "Could not create the Eidolon provisioning endpoints");
        return ESP_FAIL;
    }

    // The SDK's default success path closes Protocomm before Eidolon can
    // publish and receive its committed terminal ACK. The commissioning actor
    // is the only component allowed to stop this generation.
    if (wifi_prov_mgr_disable_auto_stop(1000) != ESP_OK) {
        ESP_LOGE(TAG, "Could not transfer transport stop ownership to runtime");
        return ESP_FAIL;
    }

    const std::string service_name = ServiceName();
    const esp_err_t err = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_2, &security_params_, service_name.c_str(), nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning did not start: %s", esp_err_to_name(err));
        return err;
    }

    // Endpoints are registered only once the service is up; the SDK requires
    // this order and unregisters them itself when provisioning stops.
    if (wifi_prov_mgr_endpoint_register(kDescriptorEndpoint, &HandleDescriptor, nullptr) != ESP_OK ||
        wifi_prov_mgr_endpoint_register(kTrustEndpoint, &HandleTrust, nullptr) != ESP_OK ||
        wifi_prov_mgr_endpoint_register(kStatusEndpoint, &HandleStatus, nullptr) != ESP_OK ||
        wifi_prov_mgr_endpoint_register(kTerminalAckEndpoint, &HandleTerminalAck, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "Could not register the Eidolon provisioning endpoints");
        return ESP_FAIL;
    }
    endpoints_registered_.store(true, std::memory_order_release);
    MaybeReportReady();
    return ESP_OK;
}

esp_err_t DeviceProvisioningService::Start(
    uint32_t generation, std::string session_id,
    ProvisioningWindowPolicy window, Events events)
{
    if (generation == 0 || session_id.empty()) return ESP_ERR_INVALID_ARG;
    if (running_.load(std::memory_order_acquire) &&
        generation == transport_generation_.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    if (!resources_.Begin(generation)) return ESP_ERR_INVALID_STATE;
    events_ = std::move(events);
    session_id_ = std::move(session_id);
    window_ = window;
    transport_generation_.store(generation, std::memory_order_release);
    cleanup_in_progress_.store(false, std::memory_order_release);
    manager_started_.store(false, std::memory_order_release);
    endpoints_registered_.store(false, std::memory_order_release);
    ready_reported_.store(false, std::memory_order_release);

    const esp_err_t worker_ready =
        OwnerTrustCommissioningWorker::GetInstance().Activate(
            transport_generation_);
    if (worker_ready != ESP_OK) {
        ESP_LOGE(TAG, "Owner trust worker did not start: %s",
                 esp_err_to_name(worker_ready));
        return worker_ready;
    }
    resources_.Own(generation,
                   CommissioningTransportResource::OwnerTrustWorker);

    // Provisioning owns the netifs while it runs and destroys them on the way
    // out, mirroring what the station does — so exactly one default Wi-Fi netif
    // of each kind exists at any moment and the handover needs no coordination
    // beyond ordering.
    sta_netif_ = esp_netif_create_default_wifi_sta();
#if !CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
    ap_netif_ = esp_netif_create_default_wifi_ap();
#endif
    // Record partial acquisition too. If one allocation succeeds and the next
    // fails, the actor's single cleanup path must still release the first.
    if (sta_netif_ != nullptr || ap_netif_ != nullptr) {
        resources_.Own(generation,
                       CommissioningTransportResource::NetworkInterfaces);
    }
    if (sta_netif_ == nullptr
#if !CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
        || ap_netif_ == nullptr
#endif
    ) {
        ESP_LOGE(TAG, "Could not allocate commissioning network interfaces");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = RegisterEventHandlers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not observe provisioning events: %s", esp_err_to_name(err));
        return err;
    }

    err = StartOwnedHttpServer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start owned commissioning HTTP server: %s",
                 esp_err_to_name(err));
        return err;
    }

    wifi_prov_mgr_config_t config = {};
#if CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
    config.scheme = wifi_prov_scheme_ble;
    config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
#else
    config.scheme = wifi_prov_scheme_softap;
    config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
#endif
    err = wifi_prov_mgr_init(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning manager did not initialize: %s", esp_err_to_name(err));
        return err;
    }
    resources_.Own(generation,
                   CommissioningTransportResource::ProvisioningManager);

    // From this point the provisioning manager may start the shared Wi-Fi
    // driver even if a later endpoint step fails. Record teardown ownership
    // before invoking it so partial start and normal stop share one path.
    resources_.Own(generation,
                   CommissioningTransportResource::WifiDriver);

    err = StartTransport();
    if (err != ESP_OK) {
        return err;
    }

    running_.store(true, std::memory_order_release);

    if (!window_.bounded) {
        // A device nobody has claimed yet keeps the offer open. Closing it
        // would leave a board that is not commissioned, cannot be
        // commissioned, and has no way to say so — recoverable only by
        // somebody standing in front of it holding a button. Security here is
        // the SRP6a setup secret the handshake already requires, not a
        // deadline; there is no Owner trust material on this device for an open
        // window to give away.
        ESP_LOGI(TAG, "Awaiting setup indefinitely: this device has no Owner yet");
        return ESP_OK;
    }

    // Arm the window this device is about to advertise. Started only once the
    // session is up, so a failed start does not leave a timer to fire into
    // nothing.
    const esp_timer_create_args_t timer_args = {
        .callback = &OnWindowElapsed,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "prov_window",
        .skip_unhandled_events = true,
    };
    auto* timer = static_cast<esp_timer_handle_t>(window_timer_);
    if (timer == nullptr) {
        const esp_err_t created = esp_timer_create(&timer_args, &timer);
        if (created != ESP_OK) {
            ESP_LOGE(TAG, "Setup window timer could not be created: %s",
                     esp_err_to_name(created));
            timer = nullptr;
        }
        window_timer_ = timer;
        if (timer != nullptr) {
            resources_.Own(generation,
                           CommissioningTransportResource::WindowTimer);
        }
    }
    // Both results are checked and said out loud. An unreported failure here is
    // what let the window stay open for as long as the board stayed powered: the
    // service announced a bounded offer it was not in fact keeping, and nothing
    // in the log contradicted it.
    if (window_timer_ != nullptr) {
        const esp_err_t armed = esp_timer_start_once(
            static_cast<esp_timer_handle_t>(window_timer_),
            static_cast<uint64_t>(window_.seconds) * 1000000ULL);
        if (armed != ESP_OK) {
            ESP_LOGE(TAG, "Setup window is not bounded: esp_timer_start_once said %s",
                     esp_err_to_name(armed));
            return armed;
        } else {
            ESP_LOGI(TAG, "Awaiting setup for %d seconds", window_.seconds);
        }
    } else {
        ESP_LOGE(TAG, "Setup window cannot be bounded");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void DeviceProvisioningService::OnWindowElapsed(void*)
{
    auto& self = GetInstance();
    if (!self.running_.load(std::memory_order_acquire)) {
        return;
    }
    // Say what closed and how to get it back. The Owner is not reading this
    // log — the projection published through window_expired reaches the panel
    // they are looking at — but the two must agree, so the gesture is named in
    // both places rather than left as tribal knowledge.
    ESP_LOGW(TAG,
             "Setup window elapsed with nobody claiming this device; "
             "long-press BOOT to reopen it");
    if (self.events_.window_expired) {
        self.events_.window_expired(
            self.transport_generation_.load(std::memory_order_acquire));
    }
}

void DeviceProvisioningService::ReleaseNetifs()
{
    if (sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
}

void DeviceProvisioningService::CleanupTransport(uint32_t generation)
{
    if (!resources_.active() || resources_.generation() != generation ||
        resources_.cleanup_claimed()) {
        return;
    }
    const CommissioningTransportCleanupPlan cleanup =
        resources_.ClaimCleanup(generation);
    cleanup_in_progress_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // Reverse acquisition order. The manager owns all protocomm endpoints and
    // must be completely deinitialized before the external HTTP server or
    // netifs disappear underneath it.
    if (cleanup.window_timer && window_timer_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(window_timer_));
        esp_timer_delete(static_cast<esp_timer_handle_t>(window_timer_));
        window_timer_ = nullptr;
    }
    if (cleanup.provisioning_manager) {
        wifi_prov_mgr_deinit();
    }
    if (cleanup.event_handlers) {
        if (security_event_instance_ != nullptr) {
            esp_event_handler_instance_unregister(
                PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                security_event_instance_);
            security_event_instance_ = nullptr;
        }
        if (provisioning_event_instance_ != nullptr) {
            esp_event_handler_instance_unregister(
                WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                provisioning_event_instance_);
            provisioning_event_instance_ = nullptr;
        }
    }
#if !CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
    wifi_prov_scheme_softap_set_httpd_handle(nullptr);
#endif
    if (cleanup.http_server && httpd_handle_ != nullptr) {
        const esp_err_t stopped =
            httpd_stop(static_cast<httpd_handle_t>(httpd_handle_));
        if (stopped != ESP_OK) {
            ESP_LOGE(TAG, "Owned commissioning HTTP server did not stop: %s",
                     esp_err_to_name(stopped));
        }
        httpd_handle_ = nullptr;
    }
    bool wifi_driver_stopped = !cleanup.wifi_driver;
    if (cleanup.wifi_driver) {
        // wifi_prov_mgr_deinit() stops Protocomm and switches APSTA to STA, but
        // deliberately leaves the driver started. Destroying its STA netif in
        // that state and then calling WifiManager::StartStation() cannot emit a
        // fresh WIFI_EVENT_STA_START, so the Station adapter never scans and
        // no route-ready evidence can exist. This is the physical RadioLease
        // handoff: the transport returns a stopped driver before releasing its
        // netifs; the Station adapter is then the sole component that starts it.
        const esp_err_t stopped = esp_wifi_stop();
        wifi_driver_stopped =
            stopped == ESP_OK || stopped == ESP_ERR_WIFI_NOT_INIT;
        if (!wifi_driver_stopped) {
            ESP_LOGE(TAG, "Owned commissioning Wi-Fi driver did not stop: %s",
                     esp_err_to_name(stopped));
        }
    }
    if (cleanup.network_interfaces &&
        cleanup.CanReleaseNetworkInterfaces(wifi_driver_stopped)) {
        ReleaseNetifs();
    }
    if (cleanup.owner_trust_worker) {
        OwnerTrustCommissioningWorker::GetInstance().Deactivate(generation);
    }

    if (!cleanup.CanReleaseNetworkInterfaces(wifi_driver_stopped)) {
        // Do not publish TransportStopped into the orchestrator: starting the
        // Station while the previous driver/netif owner is still live would
        // violate the RadioLease. Remaining here is an explicit fail-closed
        // hardware cleanup failure, not a fabricated restore completion.
        ESP_LOGE(TAG, "Provisioning generation %lu could not release its RadioLease",
                 static_cast<unsigned long>(generation));
        return;
    }

    manager_started_.store(false, std::memory_order_release);
    endpoints_registered_.store(false, std::memory_order_release);
    ready_reported_.store(false, std::memory_order_release);
    transport_generation_.store(0, std::memory_order_release);
    resources_.CompleteCleanup(generation);
    cleanup_in_progress_.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "Provisioning generation %lu stopped and released",
             static_cast<unsigned long>(generation));
    if (events_.transport_stopped) {
        events_.transport_stopped(generation);
    }
}

void DeviceProvisioningService::Stop(uint32_t generation)
{
    CleanupTransport(generation);
}

}  // namespace eidolon
