#include "guard/owner_face_engine.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "dl_image.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "hub_config_client.h"

namespace eidolon {
namespace {

constexpr char kTag[] = "OwnerFaceEngine";
constexpr char kFaceDbLabel[] = "face_db";
constexpr char kFaceDbMount[] = "/face_db";
constexpr char kFaceDbSlotA[] = "/face_db/owner_a.db";
constexpr char kFaceDbSlotB[] = "/face_db/owner_b.db";
constexpr char kLegacyFaceDbPath[] = "/face_db/owner.db";
constexpr char kOwnerFaceNvsNamespace[] = "owner_face";
constexpr uint8_t kStateCleared = 0;
constexpr uint8_t kStateActive = 1;
constexpr uint8_t kStateSchemaVersion = 1;
constexpr uint64_t kLiveIntervalMs = 1500;
constexpr uint32_t kRgb565Fourcc = static_cast<uint32_t>('R') |
                                   (static_cast<uint32_t>('G') << 8) |
                                   (static_cast<uint32_t>('B') << 16) |
                                   (static_cast<uint32_t>('R') << 24);

const char* SlotPath(uint8_t slot)
{
    return slot == 0 ? kFaceDbSlotA : kFaceDbSlotB;
}

bool MountFaceDb()
{
    if (esp_spiffs_mounted(kFaceDbLabel)) {
        return true;
    }
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = kFaceDbMount,
        .partition_label = kFaceDbLabel,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to mount Owner Face DB: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ReadNvsString(nvs_handle_t handle, const char* key, std::string& out)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length <= 1) {
        out.clear();
        return false;
    }
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[length]);
    if (!buffer || nvs_get_str(handle, key, buffer.get(), &length) != ESP_OK) {
        out.clear();
        return false;
    }
    out.assign(buffer.get());
    return true;
}

esp_err_t CommitActiveState(uint8_t slot, const OwnerFaceProfileHubConfig& profile,
                            uint32_t template_count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kOwnerFaceNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_u8(handle, "schema", kStateSchemaVersion)) == ESP_OK &&
        (err = nvs_set_u8(handle, "state", kStateActive)) == ESP_OK &&
        (err = nvs_set_u8(handle, "slot", slot)) == ESP_OK &&
        (err = nvs_set_u32(handle, "revision", profile.profile_revision)) == ESP_OK &&
        (err = nvs_set_u32(handle, "templates", template_count)) == ESP_OK &&
        (err = nvs_set_str(handle, "binding", profile.binding_id.c_str())) == ESP_OK &&
        (err = nvs_set_str(handle, "profile", profile.profile_id.c_str())) == ESP_OK &&
        (err = nvs_set_str(handle, "model", profile.model_id.c_str())) == ESP_OK &&
        (err = nvs_set_str(handle, "preproc", profile.preprocessing_version.c_str())) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t CommitClearedState(const OwnerFaceProfileHubConfig& profile)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kOwnerFaceNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_u8(handle, "schema", kStateSchemaVersion)) != ESP_OK ||
        (err = nvs_set_u8(handle, "state", kStateCleared)) != ESP_OK ||
        (err = nvs_set_u32(handle, "revision", profile.profile_revision)) != ESP_OK ||
        (err = nvs_set_u32(handle, "templates", 0)) != ESP_OK ||
        (err = nvs_set_str(handle, "binding", profile.binding_id.c_str())) != ESP_OK ||
        (err = nvs_set_str(handle, "profile", profile.profile_id.c_str())) != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    err = nvs_erase_key(handle, "model");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    err = nvs_erase_key(handle, "preproc");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t DecodeJpegRgb565BigEndian(const std::string& jpeg, uint8_t** pixels,
                                    size_t* pixels_len, size_t* width, size_t* height)
{
    size_t stride = 0;
    esp_err_t err = jpeg_to_image(
        reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), pixels,
        pixels_len, width, height, &stride);
    if (err != ESP_OK) {
        return err;
    }
    if (*pixels == nullptr || *width == 0 || *height == 0 ||
        *width > UINT16_MAX || *height > UINT16_MAX ||
        stride != *width * 2 || *pixels_len < *width * *height * 2) {
        heap_caps_free(*pixels);
        *pixels = nullptr;
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t offset = 0; offset < *width * *height * 2; offset += 2) {
        const uint8_t first = (*pixels)[offset];
        (*pixels)[offset] = (*pixels)[offset + 1];
        (*pixels)[offset + 1] = first;
    }
    return ESP_OK;
}

}  // namespace

class OwnerFaceEngine::Impl {
public:
    struct Job {
        OwnerFaceSyncRequest request;
        std::string config_url;
        std::string device_id;
        OwnerFaceApplyCallback callback;
    };

    Impl()
    {
        queue_ = xQueueCreate(2, sizeof(Job*));
        if (queue_ == nullptr ||
            xTaskCreatePinnedToCore(TaskTrampoline, "owner_face", 16384, this, 2,
                                    &task_, 1) != pdPASS) {
            ESP_LOGE(kTag, "Failed to create Owner Face worker");
            if (queue_ != nullptr) {
                vQueueDelete(queue_);
                queue_ = nullptr;
            }
            task_ = nullptr;
        }
    }

    ~Impl()
    {
        stopping_ = true;
        if (queue_ != nullptr) {
            Job* stop = nullptr;
            xQueueSend(queue_, &stop, 0);
        }
    }

    bool QueueSync(const OwnerFaceSyncRequest& request, const std::string& config_url,
                   const std::string& device_id, OwnerFaceApplyCallback callback)
    {
        if (queue_ == nullptr || request.binding_id.empty() || request.profile_id.empty() ||
            request.profile_revision == 0 ||
            (request.desired_state != "active" && request.desired_state != "cleared")) {
            return false;
        }
        std::unique_ptr<Job> job(new (std::nothrow) Job{
            request, config_url, device_id, std::move(callback)});
        if (!job) {
            return false;
        }
        Job* raw = job.get();
        if (xQueueSend(queue_, &raw, 0) != pdTRUE) {
            return false;
        }
        job.release();
        return true;
    }

    OwnerFaceLiveResult AnalyzeLiveFrame(const CameraFrame& frame, uint64_t now_ms)
    {
        OwnerFaceLiveResult result;
        if (frame.data == nullptr || frame.pixel_format != kRgb565Fourcc ||
            frame.len < static_cast<size_t>(frame.width) * frame.height * 2 ||
            now_ms < next_live_ms_) {
            return result;
        }
        next_live_ms_ = now_ms + kLiveIntervalMs;
        std::unique_lock<std::mutex> lock(model_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !recognizer_ || !detector_ || !active_) {
            return result;
        }
        dl::image::img_t image = {
            .data = const_cast<uint8_t*>(frame.data),
            .width = frame.width,
            .height = frame.height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
        };
        auto& detected = detector_->run(image);
        result.evaluated = true;
        result.evaluated_at_ms = now_ms;
        result.profile_revision = active_revision_;
        result.faces = static_cast<int>(detected.size());
        auto recognized = recognizer_->recognize(image, detected);
        if (!recognized.empty()) {
            result.id = recognized[0].id;
            result.similarity = recognized[0].similarity;
        }
        return result;
    }

    OwnerFaceProfileStatus GetProfileStatus()
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        return {
            .active = active_,
            .revision = active_revision_,
            .template_count = active_template_count_,
        };
    }

private:
    static void TaskTrampoline(void* arg)
    {
        static_cast<Impl*>(arg)->TaskLoop();
    }

    void TaskLoop()
    {
        Initialize();
        while (!stopping_) {
            Job* raw = nullptr;
            if (xQueueReceive(queue_, &raw, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (raw == nullptr) {
                break;
            }
            std::unique_ptr<Job> job(raw);
            OwnerFaceApplyResult result = Apply(*job);
            if (job->callback) {
                job->callback(result);
            }
        }
        task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void Initialize()
    {
        if (!MountFaceDb()) {
            return;
        }
        // Experimental local-button enrollment is deliberately not authoritative.
        // Product profiles must be rebuilt from an Admin-owned revision.
        remove(kLegacyFaceDbPath);
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            detector_ = std::make_unique<HumanFaceDetect>();
        }
        nvs_handle_t handle;
        if (nvs_open(kOwnerFaceNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
            remove(kFaceDbSlotA);
            remove(kFaceDbSlotB);
            return;
        }
        uint8_t schema = 0;
        uint8_t state = kStateCleared;
        uint8_t slot = 0;
        uint32_t revision = 0;
        uint32_t templates = 0;
        std::string binding;
        std::string profile;
        std::string model;
        std::string preprocessing;
        const bool common_valid = nvs_get_u8(handle, "schema", &schema) == ESP_OK &&
                                  schema == kStateSchemaVersion &&
                                  nvs_get_u8(handle, "state", &state) == ESP_OK &&
                                  nvs_get_u32(handle, "revision", &revision) == ESP_OK &&
                                  revision > 0 && ReadNvsString(handle, "binding", binding) &&
                                  ReadNvsString(handle, "profile", profile);
        if (common_valid && state == kStateCleared) {
            nvs_close(handle);
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                recognizer_.reset();
                active_ = false;
                active_revision_ = revision;
                active_template_count_ = 0;
                current_binding_id_ = std::move(binding);
                current_profile_id_ = std::move(profile);
                current_desired_state_ = "cleared";
            }
            remove(kFaceDbSlotA);
            remove(kFaceDbSlotB);
            ESP_LOGI(kTag, "Loaded cleared Owner Face state revision=%lu",
                     static_cast<unsigned long>(revision));
            return;
        }
        const bool active_valid = common_valid && state == kStateActive &&
                                  nvs_get_u8(handle, "slot", &slot) == ESP_OK && slot <= 1 &&
                                  nvs_get_u32(handle, "templates", &templates) == ESP_OK &&
                                  ReadNvsString(handle, "model", model) &&
                                  ReadNvsString(handle, "preproc", preprocessing);
        nvs_close(handle);
        if (!active_valid || templates < 3 || templates > 5 || model != kOwnerFaceModelId ||
            preprocessing != kOwnerFacePreprocessingVersion) {
            std::lock_guard<std::mutex> lock(model_mutex_);
            active_ = false;
            remove(kFaceDbSlotA);
            remove(kFaceDbSlotB);
            return;
        }
        std::string path = SlotPath(slot);
        auto recognizer = std::make_unique<HumanFaceRecognizer>(path.data());
        if (recognizer->get_num_feats() != static_cast<int>(templates)) {
            remove(kFaceDbSlotA);
            remove(kFaceDbSlotB);
            return;
        }
        std::lock_guard<std::mutex> lock(model_mutex_);
        recognizer_ = std::move(recognizer);
        active_ = true;
        active_slot_ = slot;
        active_revision_ = revision;
        active_template_count_ = templates;
        current_binding_id_ = std::move(binding);
        current_profile_id_ = std::move(profile);
        current_desired_state_ = "active";
        ESP_LOGI(kTag, "Loaded Owner Face profile revision=%lu templates=%lu slot=%u",
                 static_cast<unsigned long>(revision),
                 static_cast<unsigned long>(templates), slot);
    }

    OwnerFaceApplyResult Apply(const Job& job)
    {
        OwnerFaceApplyResult result;
        result.request = job.request;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (active_revision_ == job.request.profile_revision &&
                current_binding_id_ == job.request.binding_id &&
                current_profile_id_ == job.request.profile_id &&
                current_desired_state_ == job.request.desired_state) {
                result.success = true;
                result.code = "OK";
                if (active_ && job.request.desired_state == "active") {
                    result.model_id = kOwnerFaceModelId;
                    result.preprocessing_version = kOwnerFacePreprocessingVersion;
                    result.template_count = active_template_count_;
                }
                ESP_LOGI(kTag, "Owner Face sync already applied revision=%lu state=%s",
                         static_cast<unsigned long>(job.request.profile_revision),
                         job.request.desired_state.c_str());
                return result;
            }
        }
        OwnerFaceProfileHubConfig profile;
        HubConfigClient client;
        esp_err_t err = client.FetchOwnerFaceProfile(job.config_url, job.device_id, profile);
        if (err != ESP_OK) {
            result.code = "OWNER_FACE_MANIFEST_FETCH_FAILED";
            return result;
        }
        if (profile.binding_id != job.request.binding_id ||
            profile.profile_id != job.request.profile_id ||
            profile.profile_revision != job.request.profile_revision ||
            profile.desired_state != job.request.desired_state) {
            result.code = "OWNER_FACE_MANIFEST_MISMATCH";
            return result;
        }
        if (profile.desired_state == "cleared") {
            err = CommitClearedState(profile);
            if (err != ESP_OK) {
                result.code = "OWNER_FACE_COMMIT_FAILED";
                return result;
            }
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                recognizer_.reset();
                active_ = false;
                active_revision_ = profile.profile_revision;
                active_template_count_ = 0;
                current_binding_id_ = profile.binding_id;
                current_profile_id_ = profile.profile_id;
                current_desired_state_ = "cleared";
            }
            remove(kFaceDbSlotA);
            remove(kFaceDbSlotB);
            result.success = true;
            result.code = "OK";
            return result;
        }
        if (profile.model_id != kOwnerFaceModelId ||
            profile.preprocessing_version != kOwnerFacePreprocessingVersion) {
            result.code = "OWNER_FACE_MODEL_UNSUPPORTED";
            return result;
        }

        const uint8_t target_slot = active_ ? static_cast<uint8_t>(1 - active_slot_) : 0;
        std::string target_path = SlotPath(target_slot);
        remove(target_path.c_str());
        auto staging = std::make_unique<HumanFaceRecognizer>(target_path.data());
        std::unique_lock<std::mutex> model_lock(model_mutex_);
        for (const auto& reference : profile.references) {
            std::string jpeg;
            err = client.FetchOwnerFaceReference(
                job.config_url, job.device_id, reference, jpeg);
            if (err != ESP_OK) {
                staging.reset();
                model_lock.unlock();
                remove(target_path.c_str());
                result.code = "OWNER_FACE_REFERENCE_FETCH_FAILED";
                return result;
            }
            uint8_t* pixels = nullptr;
            size_t pixels_len = 0;
            size_t width = 0;
            size_t height = 0;
            err = DecodeJpegRgb565BigEndian(
                jpeg, &pixels, &pixels_len, &width, &height);
            jpeg.clear();
            jpeg.shrink_to_fit();
            if (err != ESP_OK) {
                staging.reset();
                model_lock.unlock();
                remove(target_path.c_str());
                result.code = "OWNER_FACE_REFERENCE_DECODE_FAILED";
                return result;
            }
            dl::image::img_t image = {
                .data = pixels,
                .width = static_cast<uint16_t>(width),
                .height = static_cast<uint16_t>(height),
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
            };
            auto& detected = detector_->run(image);
            if (detected.size() != 1 || staging->enroll(image, detected) != ESP_OK) {
                heap_caps_free(pixels);
                staging.reset();
                model_lock.unlock();
                remove(target_path.c_str());
                result.code = "OWNER_FACE_SINGLE_FACE_REQUIRED";
                return result;
            }
            heap_caps_free(pixels);
        }
        if (staging->get_num_feats() != static_cast<int>(profile.references.size())) {
            staging.reset();
            model_lock.unlock();
            remove(target_path.c_str());
            result.code = "OWNER_FACE_TEMPLATE_COUNT_MISMATCH";
            return result;
        }
        err = CommitActiveState(target_slot, profile, profile.references.size());
        if (err != ESP_OK) {
            staging.reset();
            model_lock.unlock();
            remove(target_path.c_str());
            result.code = "OWNER_FACE_COMMIT_FAILED";
            return result;
        }
        recognizer_ = std::move(staging);
        active_ = true;
        active_slot_ = target_slot;
        active_revision_ = profile.profile_revision;
        active_template_count_ = profile.references.size();
        current_binding_id_ = profile.binding_id;
        current_profile_id_ = profile.profile_id;
        current_desired_state_ = "active";
        model_lock.unlock();

        result.success = true;
        result.code = "OK";
        result.model_id = profile.model_id;
        result.preprocessing_version = profile.preprocessing_version;
        result.template_count = profile.references.size();
        ESP_LOGI(kTag, "Applied Owner Face profile revision=%lu templates=%lu slot=%u",
                 static_cast<unsigned long>(profile.profile_revision),
                 static_cast<unsigned long>(profile.references.size()), target_slot);
        return result;
    }

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool stopping_ = false;
    std::mutex model_mutex_;
    std::unique_ptr<HumanFaceDetect> detector_;
    std::unique_ptr<HumanFaceRecognizer> recognizer_;
    bool active_ = false;
    uint8_t active_slot_ = 0;
    uint32_t active_revision_ = 0;
    uint32_t active_template_count_ = 0;
    std::string current_binding_id_;
    std::string current_profile_id_;
    std::string current_desired_state_;
    uint64_t next_live_ms_ = 0;
};

OwnerFaceEngine::OwnerFaceEngine() : impl_(std::make_unique<Impl>()) {}
OwnerFaceEngine::~OwnerFaceEngine() = default;

bool OwnerFaceEngine::QueueSync(const OwnerFaceSyncRequest& request,
                                const std::string& config_url,
                                const std::string& device_id,
                                OwnerFaceApplyCallback callback)
{
    return impl_->QueueSync(request, config_url, device_id, std::move(callback));
}

OwnerFaceLiveResult OwnerFaceEngine::AnalyzeLiveFrame(const CameraFrame& frame,
                                                      uint64_t now_ms)
{
    return impl_->AnalyzeLiveFrame(frame, now_ms);
}

OwnerFaceProfileStatus OwnerFaceEngine::GetProfileStatus()
{
    return impl_->GetProfileStatus();
}

}  // namespace eidolon
