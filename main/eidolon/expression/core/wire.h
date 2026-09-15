#pragma once
#include "runtime.h"
#include <string_view>
namespace eidolon::expression {
struct WirePlan {
    std::array<char,97> presentation_id{};
    std::array<char,97> response_id{};
    Plan plan;
};
// cJSON parsing is bounded at the wire boundary; no renderer/script is selected
// by untrusted data. The caller must authenticate and bind the session first.
bool DecodePlan(std::string_view json,uint64_t token,WirePlan& output);
}
