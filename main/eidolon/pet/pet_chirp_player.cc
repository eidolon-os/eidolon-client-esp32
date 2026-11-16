#include "pet_chirp_player.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <mutex>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "eidolon/eidolon_audio_input.h"

namespace eidolon::pet {
namespace {

constexpr const char* kTag = "PetChirp";
constexpr size_t kBlock = 256;   // samples per codec write (512 B on the worker's heap)

struct Request { Chirp chirp; float volume; };

QueueHandle_t g_queue = nullptr;

void PlayLocked(AudioCodec* codec, const Request& r) {
    if (!codec) return;
    const int rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    codec->Start();
    const bool was_enabled = codec->output_enabled();
    if (!was_enabled) codec->EnableOutput(true);
    ChirpRenderer renderer(r.chirp, rate, r.volume);
    std::vector<int16_t> block(kBlock);
    while (!renderer.done()) {
        block.resize(kBlock);
        const size_t n = renderer.Render(block.data(), kBlock);
        if (n == 0) break;
        block.resize(n);
        codec->OutputData(block);
    }
    if (!was_enabled) codec->EnableOutput(false);
}

void Worker(void*) {
    Request r;
    for (;;) {
        if (xQueueReceive(g_queue, &r, portMAX_DELAY) != pdTRUE) continue;
        auto& audio = EidolonAudioInput::Instance();
        if (audio.IsReady()) {
            std::lock_guard<std::mutex> lock(audio.Mutex());
            PlayLocked(audio.Codec(), r);
        } else {
            PlayLocked(Board::GetInstance().GetAudioCodec(), r);
        }
    }
}

}  // namespace

bool PlayChirp(Chirp chirp, float volume) {
    if (chirp == Chirp::None) return false;
    if (!g_queue) {
        g_queue = xQueueCreate(1, sizeof(Request));
        if (!g_queue) return false;
        // Below the audio and network tasks, above the UI: a chirp should start promptly
        // but never compete with a conversation (and none plays during one anyway).
        if (xTaskCreate(Worker, "pet_chirp", 3072, nullptr, 3, nullptr) != pdPASS) {
            vQueueDelete(g_queue); g_queue = nullptr;
            ESP_LOGW(kTag, "worker did not start");
            return false;
        }
    }
    const Request r{chirp, volume};
    xQueueOverwrite(g_queue, &r);
    return true;
}

}  // namespace eidolon::pet
