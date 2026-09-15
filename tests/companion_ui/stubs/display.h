#pragma once
#include <cassert>
// Only the platform display lock is replaced; widgets and rendering use LVGL.
class Display {};
class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display) { assert(display); }
};
