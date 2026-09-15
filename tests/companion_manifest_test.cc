#include "eidolon/hub_onboarding_protocol.h"
#include <fstream>
#include <sstream>
#include <cassert>
int main() {
    std::ifstream file("tests/fixtures/presentation/companion-manifest.json");
    std::ostringstream buffer;buffer<<file.rdbuf();auto golden=buffer.str();
    if (!golden.empty() && golden.back()=='\n') golden.pop_back();
    assert(eidolon::BuildDeviceManifestJson("esp-box-3",false)==golden);
}
