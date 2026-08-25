#include "eidolon_view.h"

#include <utility>

namespace eidolon {

namespace {
EidolonView* g_view = nullptr;
UiIntentHandler g_intent_handler;
}

void SetEidolonView(EidolonView* view)
{
    g_view = view;
}

EidolonView* GetEidolonView()
{
    return g_view;
}

void SetEidolonUiIntentHandler(UiIntentHandler handler)
{
    g_intent_handler = std::move(handler);
}

void DispatchEidolonUiIntent(UiIntent intent)
{
    if (g_intent_handler) {
        g_intent_handler(intent);
    }
}

}  // namespace eidolon
