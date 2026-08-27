#ifndef EIDOLON_UI_STATE_MAPPER_H_
#define EIDOLON_UI_STATE_MAPPER_H_

#include "device_state.h"
#include "eidolon_runtime_status.h"
#include "eidolon_ui_model.h"

namespace eidolon {

class UiStateProjector {
public:
    static EidolonUiModel Project(const EidolonRuntimeStatus& status);
    static bool AllowsIntent(const EidolonRuntimeStatus& status, UiIntent intent);

    // The legacy DeviceState carries two unrelated things. The Application owns
    // the device's lifecycle — starting, wifi_configuring, activating,
    // upgrading — and this projection owns the conversation — connecting,
    // listening, speaking, and back to idle when it ends.
    //
    // Projecting "no conversation" as Idle wrote the second over the first: a
    // device still looking for its Host was driven activating -> idle the
    // moment the pending-approval screen was drawn, and the activation loop,
    // which read Idle as "the device is being used another way", stopped
    // asking one second later. So a target of Idle only ever ends a
    // conversation this projection started; it never moves the device out of a
    // lifecycle state, and returns `current` unchanged when there is nothing
    // of its own to end.
    static DeviceState ProjectLegacyDeviceState(const EidolonRuntimeStatus& status,
                                                DeviceState current);

    static bool IsConversationState(DeviceState state);
};

}  // namespace eidolon

#endif  // EIDOLON_UI_STATE_MAPPER_H_
