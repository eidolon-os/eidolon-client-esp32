#include <cassert>

#include "eidolon/activation_worker_resources.h"

int main() {
    assert(eidolon::kActivationWorkerStackBytes == 8 * 1024);
    assert((eidolon::kActivationWorkerMemoryCaps & MALLOC_CAP_INTERNAL) != 0);
    assert((eidolon::kActivationWorkerMemoryCaps & MALLOC_CAP_8BIT) != 0);
    assert((eidolon::kActivationWorkerMemoryCaps & MALLOC_CAP_SPIRAM) == 0);
    return 0;
}
