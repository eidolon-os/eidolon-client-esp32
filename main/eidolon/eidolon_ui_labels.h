#ifndef EIDOLON_UI_LABELS_H_
#define EIDOLON_UI_LABELS_H_

#include "eidolon_ui_types.h"

namespace eidolon {

// ASCII-only presentation policy for compact expression displays. Keeping this
// pure and separate from the presenter makes precedence rules testable without
// ESP-IDF or display hardware.
const char* EidolonBrandLabel();
const char* LifecycleLabel(LifecyclePhase phase);
const char* DefaultLifecycleDetail(LifecyclePhase phase);
const char* CompactStateLabel(const EidolonUiSnapshot& snapshot);
const char* CompactVoiceDetail(const EidolonUiSnapshot& snapshot);

}  // namespace eidolon

#endif  // EIDOLON_UI_LABELS_H_
