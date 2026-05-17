#ifndef EIDOLON_DISPLAY_HOOKS_H_
#define EIDOLON_DISPLAY_HOOKS_H_

#include <functional>

#include "eidolon_ui_types.h"

namespace eidolon {

using VoiceSessionButtonUpdater =
    std::function<void(VoiceSessionButtonState state, const char* label)>;

void SetVoiceSessionButtonUpdater(VoiceSessionButtonUpdater updater);
void UpdateVoiceSessionButton(VoiceSessionButtonState state, const char* label);

}  // namespace eidolon

#endif  // EIDOLON_DISPLAY_HOOKS_H_
