#ifndef EIDOLON_SMARTHOME_PANEL_H_
#define EIDOLON_SMARTHOME_PANEL_H_

#include <cstdint>
#include <string>

#include "smarthome_store.h"
#include "smarthome_wire.h"

// The two seams between a smart home panel view and the transport.
namespace eidolon::smarthome {

// Outbound: complete request bodies (BuildExecuteRequest / BuildSceneRequest /
// BuildSyncRequest), each to be published unchanged, reliably, on the panel's
// own channel under kPanelRequestTopic. Called on the LVGL task with the
// display lock held, so an implementation must not block and must not take the
// display lock: it hands the body off (e.g. Application::Schedule) and
// publishes from there.
class PanelSink {
public:
    virtual ~PanelSink() = default;
    virtual void SendRequest(const std::string& request_json) = 0;
};

// Inbound: what the transport wiring drives. A panel view implements it and
// serializes every call with its rendering (display lock). Parse with
// ParseMessage before calling Apply, outside the display lock.
class PanelSurface {
public:
    virtual ~PanelSurface() = default;
    // On NeedSync the surface has already asked for a snapshot through its sink.
    virtual ApplyOutcome Apply(Message&& message) = 0;
    // The channel carrying smarthome.* went down or came back. Coming back
    // also asks for a snapshot.
    virtual void SetLinkUp(bool up) = 0;
    // Trusted UTC reading (HubClock). Local time uses the snapshot's
    // utc_offset_minutes; before a snapshot the clock shows "--:--".
    virtual void SetWallClock(int64_t utc_ms) = 0;
    // nullptr restores the default (logging-only) sink.
    virtual void SetSink(PanelSink* sink) = 0;
};

}  // namespace eidolon::smarthome

#endif  // EIDOLON_SMARTHOME_PANEL_H_
