#pragma once

#include <esp_err.h>

namespace eidolon {
// Service discovery and socket name resolution share one native mDNS instance.
esp_err_t EnsureLocalMdnsInitialized();
}
