#ifndef EIDOLON_UI_STATE_MAPPER_H_
#define EIDOLON_UI_STATE_MAPPER_H_

#include "eidolon_runtime_status.h"
#include "eidolon_ui_model.h"

namespace eidolon {

class UiStateProjector {
public:
    static EidolonUiModel Project(const EidolonRuntimeStatus& status);
    static bool AllowsIntent(const EidolonRuntimeStatus& status, UiIntent intent);
};

}  // namespace eidolon

#endif  // EIDOLON_UI_STATE_MAPPER_H_
