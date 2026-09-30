#ifndef EIDOLON_HUB_ACTIVATOR_H_
#define EIDOLON_HUB_ACTIVATOR_H_

namespace eidolon {
struct Esp32HubConfig;

class HubActivator {
public:
    bool Run(Esp32HubConfig& activated);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ACTIVATOR_H_
