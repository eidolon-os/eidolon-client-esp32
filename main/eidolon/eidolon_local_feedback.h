#ifndef EIDOLON_LOCAL_FEEDBACK_H_
#define EIDOLON_LOCAL_FEEDBACK_H_

#include <esp_err.h>

namespace eidolon {

esp_err_t PlayIdentifyFeedback();
esp_err_t PlayRollCallFeedback();
// Tech-y "power-on" cue: a bright ascending arpeggio with a shimmer tail.
// Played locally on owner-presence wake (body.presence.set awake) as the audible
// half of StackChan's cute wake reaction. Synthesized PCM (no asset file).
esp_err_t PlayStartupCue();

}  // namespace eidolon

#endif  // EIDOLON_LOCAL_FEEDBACK_H_
