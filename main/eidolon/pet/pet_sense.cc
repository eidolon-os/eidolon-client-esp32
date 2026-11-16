#include "eidolon/pet/pet_sense.h"

namespace eidolon::pet {

// Stable lower-case names: they appear in logs and in whatever the runtime
// reports upstream, so renaming one is a contract change, not a refactor.
const char* SenseKindName(SenseKind kind)
{
    switch (kind) {
    case SenseKind::None: return "none";
    case SenseKind::TouchDown: return "touch_down";
    case SenseKind::TouchMove: return "touch_move";
    case SenseKind::TouchTapFront: return "touch_tap_front";
    case SenseKind::TouchTapMiddle: return "touch_tap_middle";
    case SenseKind::TouchTapBack: return "touch_tap_back";
    case SenseKind::TouchDoubleTap: return "touch_double_tap";
    case SenseKind::TouchHold: return "touch_hold";
    case SenseKind::TouchStrokeStart: return "touch_stroke_start";
    case SenseKind::TouchStrokeEnd: return "touch_stroke_end";
    case SenseKind::TouchRapid: return "touch_rapid";
    case SenseKind::TouchRelease: return "touch_release";
    case SenseKind::PickedUp: return "picked_up";
    case SenseKind::PutDown: return "put_down";
    case SenseKind::Shaken: return "shaken";
    case SenseKind::Knock: return "knock";
    case SenseKind::Tilted: return "tilted";
    case SenseKind::Level: return "level";
    case SenseKind::HandNear: return "hand_near";
    case SenseKind::FaceClose: return "face_close";
    case SenseKind::Covered: return "covered";
    case SenseKind::Uncovered: return "uncovered";
    case SenseKind::LightsOff: return "lights_off";
    case SenseKind::LightsOn: return "lights_on";
    case SenseKind::HeadPushed: return "head_pushed";
    case SenseKind::HeadReleased: return "head_released";
    case SenseKind::HeadBlocked: return "head_blocked";
    case SenseKind::ChargerIn: return "charger_in";
    case SenseKind::ChargerOut: return "charger_out";
    case SenseKind::BatteryLow: return "battery_low";
    case SenseKind::PowerKeyShort: return "power_key_short";
    }
    return "unknown";
}

}  // namespace eidolon::pet
