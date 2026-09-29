#pragma once

#include <cstddef>
#include <cstdint>
#include "eidolon_ui_model.h"

namespace eidolon {
// Input facilities and bindings are independent of turn-taking and outputs.
// A declared input is usable only after its driver has registered successfully.
enum class UiInputSource : uint8_t { Touch = 1, SessionButton = 2, TalkButton = 4, AuxiliaryButton = 8 };
enum class UiInputGesture { Click, DoubleClick, Press, Release, Cancel };
constexpr uint8_t InputBit(UiInputSource source) { return static_cast<uint8_t>(source); }
struct UiInputBinding {
    UiInputSource source;
    UiInputGesture gesture;
    UiIntent intent;
    const char* hint;
};
inline constexpr UiInputBinding kStandardUiBindings[] = {
    {UiInputSource::Touch, UiInputGesture::Click, UiIntent::OpenConversation, ""},
    {UiInputSource::Touch, UiInputGesture::Click, UiIntent::CloseConversation, ""},
    {UiInputSource::Touch, UiInputGesture::Click, UiIntent::ToggleMicrophone, ""},
    {UiInputSource::Touch, UiInputGesture::Press, UiIntent::BeginTalk, ""},
    {UiInputSource::Touch, UiInputGesture::Release, UiIntent::CommitTalk, ""},
    {UiInputSource::Touch, UiInputGesture::Cancel, UiIntent::CommitTalk, ""},
    {UiInputSource::Touch, UiInputGesture::Click, UiIntent::OpenSetup, ""},
    {UiInputSource::SessionButton, UiInputGesture::Click, UiIntent::OpenConversation, "Press button to start"},
    {UiInputSource::SessionButton, UiInputGesture::Click, UiIntent::CloseConversation, "Press button to end"},
    {UiInputSource::TalkButton, UiInputGesture::Press, UiIntent::BeginTalk, "Hold button to talk"},
    {UiInputSource::TalkButton, UiInputGesture::Release, UiIntent::CommitTalk, "Release button to send"},
    {UiInputSource::TalkButton, UiInputGesture::Cancel, UiIntent::CommitTalk, "Release button to send"},
    {UiInputSource::AuxiliaryButton, UiInputGesture::Click, UiIntent::OpenConversation, "Press button to start"},
    {UiInputSource::AuxiliaryButton, UiInputGesture::DoubleClick, UiIntent::ToggleMicrophone, "Double press to mute/unmute"},
};
struct UiInputProfile {
    uint8_t enabled_inputs = 0;
    uint8_t available_inputs = 0;
    bool automatic_start = false;
    bool setup_available = false;
    // Whether the product offers a microphone mute at all; without one no input
    // may ask for ToggleMicrophone (CONFIG_EIDOLON_UI_MICROPHONE_MUTE).
    bool microphone_mute = true;
    const UiInputBinding* bindings = kStandardUiBindings;
    size_t binding_count = sizeof(kStandardUiBindings) / sizeof(kStandardUiBindings[0]);
    bool Has(UiInputSource input) const {
        return (enabled_inputs & available_inputs & InputBit(input)) != 0;
    }
    const UiInputBinding* Binding(UiIntent intent, UiInputSource input) const {
        if (!Has(input) || (intent==UiIntent::OpenSetup && !setup_available) ||
            (intent==UiIntent::ToggleMicrophone && !microphone_mute)) return nullptr;
        for (size_t i=0; i<binding_count; ++i)
            if (bindings[i].intent==intent && bindings[i].source==input) return &bindings[i];
        return nullptr;
    }
};
// Kconfig selects declared input roles; registering a driver never enables a
// role the product did not select (e.g. BOX-3's display-only default).
inline UiInputProfile CompiledUiInputProfile(uint8_t available) {
    UiInputProfile profile;
#if CONFIG_EIDOLON_UI_TOUCH_CONTROLS
    profile.enabled_inputs |= InputBit(UiInputSource::Touch);
#endif
#if CONFIG_EIDOLON_UI_SESSION_BUTTON
    profile.enabled_inputs |= InputBit(UiInputSource::SessionButton);
#endif
#if CONFIG_EIDOLON_UI_TALK_BUTTON
    profile.enabled_inputs |= InputBit(UiInputSource::TalkButton);
#endif
#if CONFIG_EIDOLON_UI_AUXILIARY_BUTTON
    profile.enabled_inputs |= InputBit(UiInputSource::AuxiliaryButton);
#endif
#if CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION
    profile.automatic_start = true;
#endif
#if CONFIG_EIDOLON_HUB_MODE && !CONFIG_EIDOLON_UI_MICROPHONE_MUTE
    profile.microphone_mute = false;
#endif
    profile.available_inputs = available;
    return profile;
}
}  // namespace eidolon
