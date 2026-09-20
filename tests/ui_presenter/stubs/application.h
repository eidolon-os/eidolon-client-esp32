#pragma once
#include <functional>
#include "device_state.h"
class Application {
public:
    void Schedule(std::function<void()>&& fn) { fn(); }
    DeviceState GetDeviceState() const { return state_; }
    void SetDeviceState(DeviceState state) { state_=state; }
    void RequestVoiceJoin() { ++joins; }
    void RequestVoiceLeave() {}
    void PttPress() {}
    void PttRelease() {}
    void ToggleMicrophone() {}
    int joins=0;
private:
    DeviceState state_=kDeviceStateIdle;
};
