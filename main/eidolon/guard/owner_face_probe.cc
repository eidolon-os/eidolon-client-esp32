#include "guard/owner_face_probe.h"

#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_spiffs.h>

#include "boards/common/camera.h"
#include "dl_image.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "OwnerFaceProbe";
constexpr char kFaceDbLabel[] = "face_db";
constexpr char kFaceDbMount[] = "/face_db";
char kFaceDbPath[] = "/face_db/owner.db";
constexpr uint32_t kRgb565Fourcc = static_cast<uint32_t>('R') |
                                   (static_cast<uint32_t>('G') << 8) |
                                   (static_cast<uint32_t>('B') << 16) |
                                   (static_cast<uint32_t>('P') << 24);

dl::image::img_t ToDlImage(const CameraFrame& frame) {
    return {
        .data = const_cast<uint8_t*>(frame.data),
        .width = frame.width,
        .height = frame.height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
    };
}

bool MountFaceDb() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kFaceDbMount,
        .partition_label = kFaceDbLabel,
        .max_files = 2,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount owner face db: %s", esp_err_to_name(ret));
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(kFaceDbLabel, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "Owner face db mounted: total=%u used=%u",
                 static_cast<unsigned>(total), static_cast<unsigned>(used));
    } else {
        ESP_LOGW(kTag, "Owner face db mounted, size unknown: %s", esp_err_to_name(ret));
    }
    return true;
}

}  // namespace

void OwnerFaceProbe::Start(Camera& camera) {
    if (task_ != nullptr) {
        return;
    }
    camera_ = &camera;
    if (xTaskCreatePinnedToCore(Task, "owner_face", 12288, this, 2, &task_, 1) != pdPASS) {
        task_ = nullptr;
        camera_ = nullptr;
        ESP_LOGE(kTag, "Failed to start owner face probe task");
        return;
    }
    ESP_LOGI(kTag, "Owner face probe started");
}

void OwnerFaceProbe::RequestEnroll() {
    enroll_requested_.store(true);
    ESP_LOGI(kTag, "Owner enrollment requested; face the camera for the next sample");
}

void OwnerFaceProbe::RequestRecognize() {
    recognize_requested_.store(true);
    ESP_LOGI(kTag, "Owner recognition requested");
}

void OwnerFaceProbe::RequestDeleteLast() {
    delete_requested_.store(true);
    ESP_LOGI(kTag, "Owner delete-last requested");
}

void OwnerFaceProbe::Task(void* arg) {
    auto* self = static_cast<OwnerFaceProbe*>(arg);
    HumanFaceDetect detector;
    std::unique_ptr<HumanFaceRecognizer> recognizer;
    if (MountFaceDb()) {
        recognizer = std::make_unique<HumanFaceRecognizer>(kFaceDbPath);
    }
    int sample_index = 0;

    ESP_LOGI(kTag, "Human face model initialized: recognizer=%s",
             recognizer != nullptr ? "enabled" : "disabled");

    while (true) {
        if (self->camera_ == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (self->delete_requested_.exchange(false)) {
            esp_err_t ret = recognizer != nullptr ? recognizer->delete_last_feat() : ESP_ERR_INVALID_STATE;
            ESP_LOGI(kTag, "owner_face_delete result=%s enrolled=%d",
                     ret == ESP_OK ? "ok" : "failed",
                     recognizer != nullptr ? recognizer->get_num_feats() : 0);
        }

        const bool do_enroll = self->enroll_requested_.exchange(false);
        const bool do_recognize = self->recognize_requested_.exchange(false) ||
                                  (recognizer != nullptr && recognizer->get_num_feats() > 0);
        int faces = -1;
        int enrolled = recognizer != nullptr ? recognizer->get_num_feats() : 0;
        int rec_id = -1;
        float rec_similarity = 0.0f;
        esp_err_t enroll_result = ESP_ERR_NOT_FOUND;
        uint16_t width = 0;
        uint16_t height = 0;
        const int64_t started_us = esp_timer_get_time();

        const bool ok = self->camera_->AnalyzeFrame([&](const CameraFrame& frame) {
            if (frame.data == nullptr || frame.pixel_format != kRgb565Fourcc) {
                ESP_LOGW(kTag, "Unsupported frame: data=%p fourcc=0x%08lx",
                         frame.data, static_cast<unsigned long>(frame.pixel_format));
                return false;
            }
            width = frame.width;
            height = frame.height;
            auto img = ToDlImage(frame);
            auto& det_res = detector.run(img);
            faces = static_cast<int>(det_res.size());

            if (do_enroll && recognizer != nullptr) {
                enroll_result = recognizer->enroll(img, det_res);
                enrolled = recognizer->get_num_feats();
            } else if (do_recognize && enrolled > 0 && recognizer != nullptr) {
                auto rec_res = recognizer->recognize(img, det_res);
                if (!rec_res.empty()) {
                    rec_id = rec_res[0].id;
                    rec_similarity = rec_res[0].similarity;
                }
            }
            return true;
        });

        const int elapsed_ms = static_cast<int>((esp_timer_get_time() - started_us) / 1000);
        const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        if (!ok) {
            ESP_LOGW(kTag, "owner_face_sample index=%d ok=false elapsed_ms=%d", sample_index,
                     elapsed_ms);
        } else if (do_enroll) {
            ESP_LOGI(kTag,
                     "owner_face_enroll index=%d result=%s faces=%d enrolled=%d frame=%ux%u "
                     "elapsed_ms=%d internal=%u psram=%u",
                     sample_index, enroll_result == ESP_OK ? "ok" : "failed", faces, enrolled,
                     width, height, elapsed_ms, static_cast<unsigned>(internal),
                     static_cast<unsigned>(psram));
        } else if (do_recognize && enrolled > 0) {
            ESP_LOGI(kTag,
                     "owner_face_recognize index=%d faces=%d enrolled=%d id=%d sim=%.3f "
                     "frame=%ux%u elapsed_ms=%d internal=%u psram=%u",
                     sample_index, faces, enrolled, rec_id, static_cast<double>(rec_similarity),
                     width, height, elapsed_ms, static_cast<unsigned>(internal),
                     static_cast<unsigned>(psram));
        } else {
            ESP_LOGI(kTag,
                     "owner_face_detect index=%d faces=%d frame=%ux%u elapsed_ms=%d "
                     "internal=%u psram=%u",
                     sample_index, faces, width, height, elapsed_ms,
                     static_cast<unsigned>(internal), static_cast<unsigned>(psram));
        }

        ++sample_index;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_EIDOLON_ATK_OWNER_FACE_SAMPLE_INTERVAL_MS));
    }
}
