#include "eidolon_ui_presenter.h"

#include "application.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "display.h"
#include "eidolon_display_hooks.h"
#include "ui_state_mapper.h"

#include <esp_log.h>

#define TAG "EidolonUiPresenter"

namespace eidolon {

EidolonUiPresenter::EidolonUiPresenter(Application& app, Display& display)
    : app_(app), display_(display)
{
}

DeviceState EidolonUiPresenter::MapToDeviceState(VoiceSessionState session_state) const
{
    switch (session_state) {
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
        return kDeviceStateConnecting;
    case VoiceSessionState::InRoom:
        return kDeviceStateListening;
    case VoiceSessionState::ConfigReady:
    case VoiceSessionState::Idle:
    case VoiceSessionState::Error:
    default:
        return kDeviceStateIdle;
    }
}

void EidolonUiPresenter::Apply(VoiceSessionState session_state, bool mic_enabled)
{
    if (session_state == VoiceSessionState::InRoom) {
        tracker_.OnRoomConnected();
    } else if (session_state == VoiceSessionState::ConfigReady ||
               session_state == VoiceSessionState::Idle) {
        tracker_.OnRoomDisconnected();
    }

    auto snapshot = UiStateMapper::Map(session_state, tracker_.GetPhase(),
                                       tracker_.LastTranscription(), mic_enabled);
    ApplySnapshot(snapshot);

    auto device_state = MapToDeviceState(session_state);
    if (app_.GetDeviceState() != device_state) {
        app_.SetDeviceState(device_state);
    }
}

void EidolonUiPresenter::OnTranscription(const std::string& text)
{
    tracker_.OnTranscription(text);
    display_.SetChatMessage("assistant", text.c_str());
}

static const char* ButtonLabel(VoiceSessionButtonState state)
{
    switch (state) {
    case VoiceSessionButtonState::Start:
        return Lang::Strings::ROOM_START;
    case VoiceSessionButtonState::Cancel:
        return Lang::Strings::ROOM_CANCEL;
    case VoiceSessionButtonState::End:
        return Lang::Strings::ROOM_END;
    case VoiceSessionButtonState::Hidden:
    default:
        return "";
    }
}

void EidolonUiPresenter::ApplySnapshot(const EidolonUiSnapshot& snapshot)
{
    display_.SetStatus(snapshot.status_text);
    display_.SetEmotion(snapshot.emotion);

    if (snapshot.subtitle != nullptr && snapshot.subtitle[0] != '\0') {
        display_.SetChatMessage("assistant", snapshot.subtitle);
    }

    if (snapshot.button_state == VoiceSessionButtonState::Hidden) {
        UpdateVoiceSessionButton(VoiceSessionButtonState::Hidden, "");
    } else {
        UpdateVoiceSessionButton(snapshot.button_state, ButtonLabel(snapshot.button_state));
    }
}

}  // namespace eidolon
