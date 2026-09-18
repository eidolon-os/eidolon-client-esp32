#include "eidolon_view.h"

#include <utility>
#include <atomic>

namespace eidolon {

namespace {
EidolonView* g_view = nullptr;
UiIntentHandler g_intent_handler;
UiInputHandler g_input_handler;
std::function<void(UiInputSource, bool)> g_availability_handler;
std::function<void()> g_setup_handler;
std::atomic<uint8_t> g_available_inputs{0};
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

void SetEidolonUiInputHandler(UiInputHandler handler) { g_input_handler=std::move(handler); }
void DispatchEidolonUiInput(UiInputSource source, UiInputGesture gesture) {
    if (g_input_handler) g_input_handler(source,gesture);
}
void SetEidolonInputAvailabilityHandler(std::function<void(UiInputSource, bool)> handler) {
    g_availability_handler=std::move(handler);
}
void SetEidolonInputAvailable(UiInputSource source, bool available) {
    const auto bit=InputBit(source);
    const auto previous=available ? g_available_inputs.fetch_or(bit) : g_available_inputs.fetch_and(static_cast<uint8_t>(~bit));
    if (bool(previous & bit)!=available && g_availability_handler) g_availability_handler(source,available);
}
UiInputProfile CurrentUiInputProfile() {
    auto profile=CompiledUiInputProfile(g_available_inputs.load());
    profile.setup_available=bool(g_setup_handler);
    return profile;
}
void SetEidolonSetupHandler(std::function<void()> handler) { g_setup_handler=std::move(handler); }
void OpenEidolonSetup() { if (g_setup_handler) g_setup_handler(); }

}  // namespace eidolon
