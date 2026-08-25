#ifndef EIDOLON_UI_TYPES_H_
#define EIDOLON_UI_TYPES_H_

#include <string>

#include "eidolon_runtime_status.h"

namespace eidolon {

enum class TranscriptionSource {
    Unknown,
    User,
    Agent,
    System,
};

struct TranscriptionEvent {
    TranscriptionSource source = TranscriptionSource::Unknown;
    std::string text;
    bool is_final = false;
};

}  // namespace eidolon

#endif  // EIDOLON_UI_TYPES_H_
