#ifndef TEST_STUB_ESP_LOG_H_
#define TEST_STUB_ESP_LOG_H_

inline void TestEspLog(const char*, const char*, ...) {}

#define ESP_LOGD(...) TestEspLog(__VA_ARGS__)
#define ESP_LOGE(...) TestEspLog(__VA_ARGS__)
#define ESP_LOGI(...) TestEspLog(__VA_ARGS__)
#define ESP_LOGW(...) TestEspLog(__VA_ARGS__)

#endif
