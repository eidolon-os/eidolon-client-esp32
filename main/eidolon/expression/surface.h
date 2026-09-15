#pragma once
#include <functional>
#include "core/runtime.h"
namespace eidolon::expression {
class Surface {
public:
    using Observer = std::function<void(Event)>;
    virtual ~Surface() = default;
    virtual Event Submit(const Plan&) = 0;
    virtual Event Cancel(uint64_t token) = 0;
    virtual void SetObserver(Observer) = 0;
};
}
