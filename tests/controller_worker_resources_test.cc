#include <cassert>

#include "eidolon/controller_worker_resources.h"

int main() {
    assert(eidolon::kControllerWorkerStackBytes == 16 * 1024);
    return 0;
}
