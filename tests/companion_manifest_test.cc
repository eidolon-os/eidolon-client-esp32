#include "eidolon/hub_onboarding_protocol.h"
#include <fstream>
#include <cJSON.h>
#include <cstring>
#include "eidolon/device_capabilities.h"
#include "eidolon/output_policy.h"
#include <sstream>
#include <cassert>
int main() {
    std::ifstream file("tests/fixtures/presentation/companion-manifest.json");
    std::ostringstream buffer;buffer<<file.rdbuf();auto golden=buffer.str();
    if (!golden.empty() && golden.back()=='\n') golden.pop_back();
    assert(eidolon::BuildDeviceManifestJson("esp-box-3",false)==golden);
    const auto compiled=eidolon::CompiledDeviceCapabilities();
    assert(compiled.dialogue_text && compiled.expression);
    assert(eidolon::CurrentOutputGate().Bind({true,1,compiled.OutputMask()}));
    assert(eidolon::CurrentOutputGate().Start({"compiled",1,
        eidolon::OutputBit(eidolon::presentation::Output::DialogueText),false},"compiled"));
    assert(eidolon::CurrentOutputGate().Allows(eidolon::presentation::Output::DialogueText));
    // A new text-only device uses the same serializer without a board-name branch.
    eidolon::DeviceCapabilities text;
    text.speaker=false;text.dialogue_text=true;
    auto manifest=eidolon::BuildDeviceManifestJson("text-fixture",text);
    auto* json=cJSON_Parse(manifest.c_str());assert(json);
    auto* media=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json,"media"),0);
    assert(!std::strcmp(cJSON_GetObjectItemCaseSensitive(media,"direction")->valuestring,"publish"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(json,"actions"))==0);
    assert(manifest.find("output.dialogue_text")!=std::string::npos);
    assert(manifest.find("expression.profile")==std::string::npos);
    cJSON_Delete(json);
    text.microphone=false;text.camera=true;
    manifest=eidolon::BuildDeviceManifestJson("camera-fixture",text);
    json=cJSON_Parse(manifest.c_str());assert(json);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(json,"media"))==1);
    assert(manifest.find("opus")==std::string::npos);
    cJSON_Delete(json);
    text.camera=false;text.speaker=true;text.audio_cue=true;
    manifest=eidolon::BuildDeviceManifestJson("playback-fixture",text);
    assert(manifest.find("subscribe")!=std::string::npos);
    assert(manifest.find("output.audio_cue")!=std::string::npos);
    text.speaker=false;
    manifest=eidolon::BuildDeviceManifestJson("no-audio-fixture",text);
    json=cJSON_Parse(manifest.c_str());assert(json);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(json,"media"))==0);
    assert(manifest.find("output.audio_cue")==std::string::npos);
    cJSON_Delete(json);
}
