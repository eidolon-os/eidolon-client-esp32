#pragma once
#include <cstdint>

namespace eidolon::companion::palette {
// Warm paper, evergreen stage and mint expression. Flat fills keep the theme
// inexpensive on SPI displays; no bitmap backgrounds or blur/shadow buffers.
inline constexpr uint32_t Canvas = 0xEFEDE5;
inline constexpr uint32_t Paper = 0xFBFAF5;
inline constexpr uint32_t Ink = 0x263F38;
inline constexpr uint32_t Muted = 0x51665C;
inline constexpr uint32_t Stage = 0x193F36;
inline constexpr uint32_t StageEdge = 0x365A4E;
inline constexpr uint32_t Expression = 0xC5ECCD;
inline constexpr uint32_t Soft = 0xDBE5D9;
inline constexpr uint32_t Accent = 0x32745C;
inline constexpr uint32_t Pressed = 0x245541;
inline constexpr uint32_t Attention = 0x996321;
inline constexpr uint32_t Error = 0xA64538;
inline constexpr uint32_t ErrorSoft = 0xF0DFD5;
}
