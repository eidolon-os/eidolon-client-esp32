#pragma once
#include <cstdint>
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

namespace eidolon::companion::palette {
#if CONFIG_EIDOLON_PET_FACE
// Desk pet (StackChan): dark chrome so the glowing face owns the screen; warm amber
// controls. Same roles, so every widget keeps its meaning (design §4.11).
inline constexpr uint32_t Canvas = 0x121316;
inline constexpr uint32_t Paper = 0x1E2026;
inline constexpr uint32_t Ink = 0xEEE9DE;
inline constexpr uint32_t Muted = 0x9AA0A8;
inline constexpr uint32_t Stage = 0x121316;
inline constexpr uint32_t StageEdge = 0x2A2D33;
inline constexpr uint32_t Expression = 0xF2ECDF;
inline constexpr uint32_t Accent = 0xE8C27A;
inline constexpr uint32_t Pressed = 0xC9A35E;
inline constexpr uint32_t Attention = 0xFFB547;
inline constexpr uint32_t Error = 0xFF8A75;
inline constexpr uint32_t ErrorSoft = 0x3A2624;
#else
// Warm paper, evergreen stage and mint expression. Flat fills keep the theme
// inexpensive on SPI displays; no bitmap backgrounds or blur/shadow buffers.
inline constexpr uint32_t Canvas = 0xEFEDE5;
inline constexpr uint32_t Paper = 0xFBFAF5;
inline constexpr uint32_t Ink = 0x263F38;
inline constexpr uint32_t Muted = 0x51665C;
inline constexpr uint32_t Stage = 0x193F36;
inline constexpr uint32_t StageEdge = 0x365A4E;
inline constexpr uint32_t Expression = 0xC5ECCD;
inline constexpr uint32_t Accent = 0x32745C;
inline constexpr uint32_t Pressed = 0x245541;
inline constexpr uint32_t Attention = 0x996321;
inline constexpr uint32_t Error = 0xA64538;
inline constexpr uint32_t ErrorSoft = 0xF0DFD5;
#endif
}
