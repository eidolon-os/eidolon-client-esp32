#ifndef EIDOLON_UI_LABELS_H_
#define EIDOLON_UI_LABELS_H_

#include "eidolon_ui_model.h"

namespace eidolon {

const char* EidolonBrandLabel();
const char* InteractionModeLabel(InteractionMode mode);
const char* UiSceneLabel(UiScene scene);
const char* UiSceneStatus(UiScene scene);
const char* UiSceneDetail(UiScene scene, EndReason end_reason = EndReason::None);
const char* UiSceneEmotion(UiScene scene);

}  // namespace eidolon

#endif  // EIDOLON_UI_LABELS_H_
