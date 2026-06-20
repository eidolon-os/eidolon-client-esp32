#include "eidolon_view.h"

namespace eidolon {

namespace {
EidolonView* g_view = nullptr;
}

void SetEidolonView(EidolonView* view)
{
    g_view = view;
}

EidolonView* GetEidolonView()
{
    return g_view;
}

}  // namespace eidolon
