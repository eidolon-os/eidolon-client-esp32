#pragma once

// Shared vocabulary between StackChan body sensors and the pet runtime.
// Pure data: no ESP-IDF, no LVGL, so recognizers and the runtime are host-testable.
// Raw samples come from board drivers; recognizers turn them into SenseEvents,
// which are local judgements ("picked up", "stroked with the fur") and are never
// uploaded or stored.

#include <cstdint>

namespace eidolon::pet {

// Si12T head-top capacitive pad: three electrodes, front (towards the face),
// middle, back. Each intensity is the chip's 2-bit level, 0..3.
struct HeadTouchSample {
    uint64_t t_ms = 0;
    uint8_t front = 0, middle = 0, back = 0;
};

// BMI270 accelerometer in g (gyro optional, deg/s, 0 when unavailable).
// Axes as the chip reports them on CoreS3; recognizers only rely on the
// magnitude and on the change of the gravity direction.
struct ImuSample {
    uint64_t t_ms = 0;
    float ax = 0, ay = 0, az = 0;
    float gx = 0, gy = 0, gz = 0;
};

// LTR-553: proximity raw count (0..2047, larger = nearer) and ambient light.
struct LightSample {
    uint64_t t_ms = 0;
    uint16_t proximity = 0;
    float lux = 0;
    bool proximity_valid = false;
    bool lux_valid = false;
};

enum class SenseKind : uint8_t {
    None,
    // Head-top touch (Si12T).
    TouchDown,              // value = position -1 (front) .. +1 (back)
    TouchMove,              // continuous while held; value = position, dir = -1/0/+1 recent motion
    TouchTapFront,
    TouchTapMiddle,
    TouchTapBack,
    TouchDoubleTap,
    TouchHold,              // held >= 800 ms without travelling
    TouchStrokeStart,       // dir = +1 front->back (with the fur), -1 back->front (against)
    TouchStrokeEnd,         // dir as above; value = seconds stroked
    TouchRapid,             // >= 4 taps within 2 s
    TouchRelease,
    // IMU (BMI270).
    PickedUp,
    PutDown,
    Shaken,
    Knock,                  // short impulse without a change of attitude (tap on the desk / body)
    Tilted,                 // resting at an angle; value = degrees
    Level,                  // back to resting level
    // Proximity / ambient light (LTR-553).
    HandNear,
    FaceClose,
    Covered,                // proximity saturated for a while (peekaboo)
    Uncovered,
    LightsOff,
    LightsOn,
    // Servo readback (StackChan body).
    HeadPushed,             // value = degrees displaced by a hand
    HeadReleased,
    HeadBlocked,
    // Power (AXP2101).
    ChargerIn,
    ChargerOut,
    BatteryLow,
    PowerKeyShort,
};

struct SenseEvent {
    SenseKind kind = SenseKind::None;
    uint64_t t_ms = 0;
    float value = 0;
    int8_t dir = 0;
};

const char* SenseKindName(SenseKind kind);

}  // namespace eidolon::pet
