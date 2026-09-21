#include "eidolon/audio/pcm_push_capture_source.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using eidolon::PcmPushCaptureSource;
int main() {
 PcmPushCaptureSource source;
 auto* api=source.Interface();
 assert(api->open(api)==ESP_CAPTURE_ERR_OK);
 assert(api->start(api)==ESP_CAPTURE_ERR_OK);
 std::array<int16_t,160> known; known.fill(0x1234);
 for(int cycle=0;cycle<5;++cycle) {
  for(int i=0;i<50;++i) source.Push(known.data(),known.size());
  std::vector<int16_t> drained(7999);
  esp_capture_stream_frame_t frame{};
  frame.data=reinterpret_cast<uint8_t*>(drained.data()); frame.size=15998;
  assert(api->read_frame(api,&frame)==ESP_CAPTURE_ERR_OK);
  for(auto sample:drained) assert(sample==0x1234);
  source.Push(known.data(),known.size());
  std::array<int16_t,160> after{};
  frame.data=reinterpret_cast<uint8_t*>(after.data()); frame.size=320;
  assert(api->read_frame(api,&frame)==ESP_CAPTURE_ERR_OK);
  for(auto sample:after) assert(sample==0x1234);
  source.Flush();
 }
 // An odd request must not consume half a sample and poison all subsequent frames.
 source.Push(known.data(),known.size());
 std::array<uint8_t,320> bytes{}; esp_capture_stream_frame_t frame{};
 frame.data=bytes.data(); frame.size=1;
 assert(api->read_frame(api,&frame)==ESP_CAPTURE_ERR_INVALID_ARG);
 frame.size=320; assert(api->read_frame(api,&frame)==ESP_CAPTURE_ERR_OK);
 for(size_t i=0;i<bytes.size();i+=2) assert(bytes[i]==0x34 && bytes[i+1]==0x12);
 api->stop(api); api->start(api);
 source.Push(known.data(),known.size()); frame.size=320;
 assert(api->read_frame(api,&frame)==ESP_CAPTURE_ERR_OK);
 assert(frame.pts==0);
 api->stop(api);
 puts("PCM overflow, sample boundaries, odd request, and restart: PASS");
}
