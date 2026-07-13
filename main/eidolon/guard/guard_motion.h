#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "boards/common/camera.h"

namespace eidolon {

constexpr size_t kGuardMotionGridWidth = 24;
constexpr size_t kGuardMotionGridHeight = 18;
constexpr size_t kGuardMotionGridPixels = kGuardMotionGridWidth * kGuardMotionGridHeight;

using GuardLuminanceGrid = std::array<uint8_t, kGuardMotionGridPixels>;

bool ReadGuardLuminanceGrid(const CameraFrame& frame, GuardLuminanceGrid& output);
uint32_t GuardMotionScore(const GuardLuminanceGrid& before, const GuardLuminanceGrid& after);
void GuardFourcc(uint32_t value, char output[5]);

}  // namespace eidolon
