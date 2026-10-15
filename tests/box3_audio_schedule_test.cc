#include "eidolon/audio/box3_audio_schedule.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

struct ThreadConfig {
    uint8_t priority;
    uint8_t core_id;
    uint32_t stack_size;
};
int main() {
    // The adapter runs after the SDK callback, so SDK-selected stack/priority
    // stack sizes (including future values) must survive without being duplicated.
    for (uint8_t priority : {uint8_t(3), uint8_t(15), uint8_t(20)}) {
        for (uint32_t stack : {4096u, 40960u, 65536u}) {
            ThreadConfig config{priority, 0, stack};
            eidolon::audio::ApplyBox3AudioPlacement("Adec", config);
            assert(config.core_id == 1);
            assert(config.priority == 10 && config.stack_size == stack);
            for (const char* name : {"aenc_0", "ARender", "lk_peer_sub", "future_sdk_task", "Adec_extra", ""}) {
                ThreadConfig unchanged{priority, 0, stack};
                eidolon::audio::ApplyBox3AudioPlacement(name, unchanged);
                assert(unchanged.core_id == 0 && unchanged.priority == priority && unchanged.stack_size == stack);
            }
        }
    }
    ThreadConfig null_name{18,1,25000};
    eidolon::audio::ApplyBox3AudioPlacement(nullptr, null_name);
    assert(null_name.core_id == 1 && null_name.priority == 18 && null_name.stack_size == 25000);
}
