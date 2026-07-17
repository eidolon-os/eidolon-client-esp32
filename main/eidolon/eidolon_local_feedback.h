#ifndef EIDOLON_LOCAL_FEEDBACK_H_
#define EIDOLON_LOCAL_FEEDBACK_H_

#include <esp_err.h>

namespace eidolon {

esp_err_t PlayIdentifyFeedback();
esp_err_t PlayRollCallFeedback();

}  // namespace eidolon

#endif  // EIDOLON_LOCAL_FEEDBACK_H_
