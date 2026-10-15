#include "sdkconfig.h"
#if CONFIG_EIDOLON_HUB_MODE && CONFIG_BOARD_TYPE_ESP_BOX_3 && CONFIG_USE_DEVICE_AEC
#include "box3_audio_schedule.h"
#include <media_lib_os.h>
#include <atomic>

namespace {
// LiveKit installs this public callback during one-time system initialization,
// before media tasks are created. Chain it instead of copying its task table:
// SDK stack changes and unknown task names remain authoritative.
std::atomic<media_lib_thread_sched_param_cb> upstream_schedule{nullptr};
void Schedule(const char* name, media_lib_thread_cfg_t* config) {
    if (auto callback=upstream_schedule.load(std::memory_order_acquire)) callback(name, config);
    eidolon::audio::ApplyBox3AudioPlacement(name, *config);
}
}

extern "C" {
void __real_media_lib_thread_set_schedule_cb(media_lib_thread_sched_param_cb);
// The application owns BOX-3's full-duplex CPU budget. This link-time adapter
// leaves the SDK and managed components unmodified, changing Adec's core and priority
// after the SDK has selected its stack. Enable only for this board.
void __wrap_media_lib_thread_set_schedule_cb(media_lib_thread_sched_param_cb callback) {
    upstream_schedule.store(callback, std::memory_order_release);
    __real_media_lib_thread_set_schedule_cb(Schedule);
}
}
#endif
