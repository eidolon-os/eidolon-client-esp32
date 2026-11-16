#ifndef EIDOLON_POLICY_ANNOUNCEMENT_H_
#define EIDOLON_POLICY_ANNOUNCEMENT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace eidolon {

// This device learns that its Owner changed policy from an update to its own
// LiveKit participant. The Channel Provider writes the committed policy
// revision onto that participant as this attribute with every revision
// (eidolon_channel, LiveKit adapter POLICY_REVISION_ATTRIBUTE). A Host from
// before 2026-11-16 writes nothing, and then an update is only a bare hint.
inline constexpr char kPolicyRevisionAttribute[] = "eidolon.policy_revision";

struct PolicyAnnouncement {
    bool announced = false;
    uint32_t revision = 0;
};

// Decimal digits that fit 32 bits, nothing else: a value this device cannot
// read announces nothing rather than a revision it made up.
inline bool ParsePolicyRevision(const char* text, uint32_t* out)
{
    if (!text || !out || *text == '\0') return false;
    uint64_t value = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(*c - '0');
        if (value > UINT32_MAX) return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

// `Attribute` is anything with `const char* key, value` (the LiveKit SDK's
// livekit_participant_attribute_t on the device).
template <typename Attribute>
PolicyAnnouncement FindPolicyAnnouncement(const Attribute* attributes, size_t count)
{
    PolicyAnnouncement found;
    if (!attributes) return found;
    for (size_t i = 0; i < count; ++i) {
        if (attributes[i].key && std::strcmp(attributes[i].key, kPolicyRevisionAttribute) == 0) {
            found.announced = ParsePolicyRevision(attributes[i].value, &found.revision);
            return found;
        }
    }
    return found;
}

// Participant updates arrive on the LiveKit task; the controller reads them
// later, once, however many arrived in between. A bare hint outweighs any
// announcement because only fetching can answer it; otherwise the latest
// announced revision is the one that matters.
class ConfigurationHintLatch {
public:
    struct Taken {
        bool pending = false;
        bool bare = false;
        uint32_t revision = 0;
    };

    void Note(const PolicyAnnouncement& announcement)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!announcement.announced) {
            pending_.bare = true;
        } else {
            pending_.revision = announcement.revision;
        }
        pending_.pending = true;
    }

    Taken Take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Taken taken = pending_;
        pending_ = {};
        return taken;
    }

private:
    std::mutex mutex_;
    Taken pending_;
};

// An update that announces the revision this device already holds restates
// what it fetched before joining — every (re)join produces one or two — and
// fetching again would cost the controller seconds for nothing. Any other
// revision, including a lower one after the Host's Authority was reset, is
// fetched.
inline bool HintCallsForConfiguration(const ConfigurationHintLatch::Taken& taken,
                                      uint32_t held_revision)
{
    return taken.pending && (taken.bare || taken.revision != held_revision);
}

}  // namespace eidolon

#endif  // EIDOLON_POLICY_ANNOUNCEMENT_H_
