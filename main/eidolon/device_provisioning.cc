#include "device_provisioning.h"

#include "device_identity.h"
#include "device_provisioning_protocol.h"
#include "hub_trust_store.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#if CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
#include <wifi_provisioning/scheme_ble.h>
#else
#include <wifi_provisioning/scheme_softap.h>
#endif

#include <ssid_manager.h>

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

// How long this device offers to be set up. A window rather than a permanent
// door: an uncommissioned device that never closed would let anything on the
// network claim it, and every commissioning standard bounds this the same way.
// The controller receives it as a duration and computes its own expiry.
constexpr int kWindowSeconds = CONFIG_EIDOLON_PROVISIONING_WINDOW_SECONDS;

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

std::string RandomToken(size_t bytes)
{
    static const char* kAlphabet = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string token;
    token.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        token.push_back(kAlphabet[esp_random() % 36]);
    }
    return token;
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
    ProvisioningDescriptor descriptor;
    descriptor.device_id = SystemInfo::GetMacAddress();
    descriptor.device_kind = BOARD_TYPE;
    descriptor.display_name = BOARD_NAME;
    descriptor.identity_fingerprint = DeviceIdentity::GetInstance().Fingerprint();
    descriptor.session_id = self.session_id_;
    descriptor.expires_in_seconds = kWindowSeconds;
    // This build carries a shared development secret unless it was given a
    // per-device one, and says so rather than letting the controller assume.
#ifdef CONFIG_EIDOLON_PROVISIONING_MANUFACTURER_BOUND
    descriptor.manufacturer_bound = true;
#else
    descriptor.manufacturer_bound = false;
#endif
    return Answer(BuildProvisioningDescriptorJson(descriptor), outbuf, outlen);
}

esp_err_t DeviceProvisioningService::HandleTrust(uint32_t, const uint8_t* inbuf, ssize_t inlen,
                                                uint8_t** outbuf, ssize_t* outlen, void*)
{
    const std::string body(reinterpret_cast<const char*>(inbuf != nullptr ? inbuf : nullptr),
                           inlen > 0 ? static_cast<size_t>(inlen) : 0);
    TrustHandover handover;
    if (!ParseTrustHandover(body, handover)) {
        ESP_LOGE(TAG, "Refused a trust handover this firmware does not understand");
        return Answer(BuildTrustRefusedJson("handover is not supported"), outbuf, outlen);
    }
    const esp_err_t saved = HubTrustStore().Save(handover.hub_id, handover.certificate_pem);
    if (saved != ESP_OK) {
        ESP_LOGE(TAG, "Rejected commissioned certificate: %s", esp_err_to_name(saved));
        return Answer(BuildTrustRefusedJson("certificate was not stored"), outbuf, outlen);
    }
    ESP_LOGI(TAG, "Commissioned for Hub %s", handover.hub_id.c_str());
    return Answer(BuildTrustAcceptedJson(SystemInfo::GetMacAddress(), handover.hub_id), outbuf,
                  outlen);
}

void DeviceProvisioningService::HandleProvisioningEvent(void*, const char*, int32_t event_id,
                                                       void* event_data)
{
    auto& self = GetInstance();
    switch (event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "Provisioning session open as %s", ServiceName().c_str());
        break;
    case WIFI_PROV_CRED_RECV: {
        // The Wi-Fi driver is initialized with NVS persistence disabled, so the
        // credentials the SDK just applied live only in RAM. SsidManager is this
        // firmware's one credential store, and writing here is what makes the
        // network survive a reboot.
        const auto* config = static_cast<const wifi_sta_config_t*>(event_data);
        if (config != nullptr) {
            const std::string ssid(reinterpret_cast<const char*>(config->ssid),
                                   strnlen(reinterpret_cast<const char*>(config->ssid),
                                           sizeof(config->ssid)));
            const std::string password(reinterpret_cast<const char*>(config->password),
                                       strnlen(reinterpret_cast<const char*>(config->password),
                                               sizeof(config->password)));
            ESP_LOGI(TAG, "Provisioned network %s", ssid.c_str());
            SsidManager::GetInstance().AddSsid(ssid, password);
        }
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        // Unlike the endpoint this replaces, a wrong password is reported to the
        // controller while it is still connected, before anything is torn down.
        ESP_LOGW(TAG, "Provisioned network did not come up; controller was told");
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioned network came up");
        break;
    case WIFI_PROV_END:
        // Provisioning owned the radio while it ran. Give it back.
        self.Stop();
        if (self.handover_) {
            self.handover_();
        }
        break;
    default:
        break;
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
        wifi_prov_mgr_endpoint_create(kTrustEndpoint) != ESP_OK) {
        ESP_LOGE(TAG, "Could not create the Eidolon provisioning endpoints");
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
        wifi_prov_mgr_endpoint_register(kTrustEndpoint, &HandleTrust, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "Could not register the Eidolon provisioning endpoints");
        wifi_prov_mgr_stop_provisioning();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t DeviceProvisioningService::Start(StationHandover handover)
{
    if (running_) {
        return ESP_OK;
    }
    handover_ = std::move(handover);
    session_id_ = RandomToken(16);

    // The device identity has to exist before a controller reads the descriptor:
    // its fingerprint is what the controller matches the later enrollment
    // against, so producing it lazily would let setup start against a device
    // that cannot yet prove who it is.
    if (DeviceIdentity::GetInstance().EnsureKeypair() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot offer provisioning without a device identity");
        return ESP_FAIL;
    }

    // Provisioning owns the netifs while it runs and destroys them on the way
    // out, mirroring what the station does — so exactly one default Wi-Fi netif
    // of each kind exists at any moment and the handover needs no coordination
    // beyond ordering.
    sta_netif_ = esp_netif_create_default_wifi_sta();
#if !CONFIG_EIDOLON_PROVISIONING_TRANSPORT_BLE
    ap_netif_ = esp_netif_create_default_wifi_ap();
#endif

    esp_err_t err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                              &HandleProvisioningEvent, nullptr);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Could not observe provisioning events: %s", esp_err_to_name(err));
        ReleaseNetifs();
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
        ReleaseNetifs();
        return err;
    }

    err = StartTransport();
    if (err != ESP_OK) {
        wifi_prov_mgr_deinit();
        ReleaseNetifs();
        return err;
    }

    running_ = true;

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
    }
    // Both results are checked and said out loud. An unreported failure here is
    // what let the window stay open for as long as the board stayed powered: the
    // service announced a bounded offer it was not in fact keeping, and nothing
    // in the log contradicted it.
    if (window_timer_ != nullptr) {
        const esp_err_t armed = esp_timer_start_once(
            static_cast<esp_timer_handle_t>(window_timer_),
            static_cast<uint64_t>(kWindowSeconds) * 1000000ULL);
        if (armed != ESP_OK) {
            ESP_LOGE(TAG, "Setup window is not bounded: esp_timer_start_once said %s",
                     esp_err_to_name(armed));
        } else {
            ESP_LOGI(TAG, "Awaiting setup for %d seconds", kWindowSeconds);
        }
    } else {
        ESP_LOGE(TAG, "Setup window cannot be bounded; it will stay open until reset");
    }
    return ESP_OK;
}

void DeviceProvisioningService::OnWindowElapsed(void*)
{
    auto& self = GetInstance();
    if (!self.running_) {
        return;
    }
    ESP_LOGI(TAG, "Setup window elapsed with nobody claiming this device");
    self.Stop();
    if (self.handover_) {
        self.handover_();
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

void DeviceProvisioningService::Stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    if (window_timer_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(window_timer_));
    }
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    ReleaseNetifs();
    ESP_LOGI(TAG, "Provisioning session closed");
}

}  // namespace eidolon
