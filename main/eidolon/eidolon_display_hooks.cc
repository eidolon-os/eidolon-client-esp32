#include "eidolon_display_hooks.h"

namespace eidolon {

namespace {
VoiceSessionButtonUpdater g_updater;
}

void SetVoiceSessionButtonUpdater(VoiceSessionButtonUpdater updater)
{
    g_updater = std::move(updater);
}

void UpdateVoiceSessionButton(VoiceSessionButtonState state, const char* label)
{
    if (g_updater) {
        g_updater(state, label);
    }
}

}  // namespace eidolon
