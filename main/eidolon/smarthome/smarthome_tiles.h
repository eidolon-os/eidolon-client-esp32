#ifndef EIDOLON_SMARTHOME_TILES_H_
#define EIDOLON_SMARTHOME_TILES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "smarthome_model.h"
#include "smarthome_store.h"

// What a panel shows for the cached home and what a touch asks for, as pure
// functions of the cache. The panel never predicts the outcome of a command:
// a tile changes only when the host reports the new state.
namespace eidolon::smarthome {

enum class TileAction : uint8_t { Tap, Minus, Plus };

// One CJK character naming the device on its tile (UTF-8).
const char* DeviceGlyph(const Device& device);
// Chinese state line, e.g. "开 · 40%", "制冷 26°C · 室温 28°", "已关闭".
// `compact` drops secondary detail (a climate's room temperature) for a tile
// too narrow to show it whole.
std::string DeviceStateText(const Device& device, bool compact = false);
// Drawn highlighted: on, open, running, or (for a lock) unlocked.
bool IsActive(const Device& device);
// Whether the tile offers - / + controls.
bool OffersSteps(const Device& device);
// The command a touch produces, computed from the cached state where the wire
// has no relative form (position, fan speed). False: nothing to send.
bool TileCommand(const Device& device, TileAction action, Command& out);

// Activity line wording for one change, comparing the cached state with the
// reported one, e.g. "已打开", "开到 70%", "设为 24°C".
std::string DescribeChange(const Device& before, const Change& after);
const char* OriginLabel(OriginKind kind);
// How the result card colors an outcome: answers and completed commands read
// normally, a partial success asks for attention, the rest are failures.
enum class ResultTone : uint8_t { Normal, Attention, Error };
ResultTone OutcomeTone(VoiceOutcome outcome);

// "HH:MM" in the given offset (the snapshot's utc_offset_minutes), or "--:--"
// before the panel has a wall clock.
std::string FormatClock(int64_t utc_ms, int32_t utc_offset_minutes);
// "HH:MM 客厅空调 已打开 · 来自 面板语音"; the time is left out without a clock.
std::string FormatActivity(const Activity& activity, int32_t utc_offset_minutes);

// Area navigation: "全部", then the snapshot's areas that hold devices, then
// "其他" for devices placed in no area.
inline constexpr const char* kAllAreas = "";
inline constexpr const char* kUnplacedArea = "~unplaced";  // never an Identifier
struct NavEntry {
    std::string area_id;
    std::string name;
    size_t count = 0;
};
std::vector<NavEntry> BuildNav(const Snapshot& home);
// The area a panel opens on: its own area when it holds devices, else all.
std::string DefaultArea(const Snapshot& home);
bool InArea(const Device& device, const std::string& area_id);

}  // namespace eidolon::smarthome

#endif  // EIDOLON_SMARTHOME_TILES_H_
