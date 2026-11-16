#ifndef TEST_LIVEKIT_WRITER_ESP_LOG_H_
#define TEST_LIVEKIT_WRITER_ESP_LOG_H_
// The SDK's C sources log through esp_log; on the host it goes nowhere.
static inline void TestLivekitLog(const char* tag, const char* format, ...) { (void)tag; (void)format; }
#define ESP_LOGD(...) TestLivekitLog(__VA_ARGS__)
#define ESP_LOGE(...) TestLivekitLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLivekitLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLivekitLog(__VA_ARGS__)
#endif
