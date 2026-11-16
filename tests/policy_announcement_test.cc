// What a device does with the Owner policy revision its own participant
// update announces: read it strictly, coalesce updates between two reads by
// the controller, and fetch configuration only for a revision it does not hold.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "eidolon/policy_announcement.h"

namespace {

using eidolon::ConfigurationHintLatch;
using eidolon::FindPolicyAnnouncement;
using eidolon::HintCallsForConfiguration;
using eidolon::ParsePolicyRevision;
using eidolon::PolicyAnnouncement;

struct Attribute {
    const char* key;
    const char* value;
};

PolicyAnnouncement Announced(uint32_t revision)
{
    PolicyAnnouncement announcement;
    announcement.announced = true;
    announcement.revision = revision;
    return announcement;
}

void ParsesOnlyPlainDecimal()
{
    uint32_t value = 7;
    assert(ParsePolicyRevision("0", &value) && value == 0);
    assert(ParsePolicyRevision("10", &value) && value == 10);
    assert(ParsePolicyRevision("4294967295", &value) && value == UINT32_MAX);
    value = 7;
    for (const char* bad : {"", "4294967296", "99999999999999999999", "-1", "+1", " 1", "1 ",
                            "1.0", "0x10", "9a"}) {
        assert(!ParsePolicyRevision(bad, &value));
    }
    assert(value == 7);
    assert(!ParsePolicyRevision(nullptr, &value));
    assert(!ParsePolicyRevision("1", nullptr));
}

void FindsTheRevisionAmongOtherAttributes()
{
    const Attribute agent_and_policy[] = {
        {"lk.agent.state", "listening"}, {nullptr, "x"}, {"eidolon.policy_revision", "12"}};
    PolicyAnnouncement found = FindPolicyAnnouncement(agent_and_policy, 3);
    assert(found.announced && found.revision == 12);

    // An older Host writes nothing; a value this device cannot read is the
    // same as none, never a guessed revision.
    const Attribute others[] = {{"lk.agent.state", "speaking"}};
    assert(!FindPolicyAnnouncement(others, 1).announced);
    const Attribute unreadable[] = {{"eidolon.policy_revision", "twelve"}};
    assert(!FindPolicyAnnouncement(unreadable, 1).announced);
    const Attribute empty[] = {{"eidolon.policy_revision", nullptr}};
    assert(!FindPolicyAnnouncement(empty, 1).announced);
    assert(!FindPolicyAnnouncement(static_cast<const Attribute*>(nullptr), 4).announced);
    assert(!FindPolicyAnnouncement(agent_and_policy, 0).announced);
}

void RestatedRevisionIsNotFetched()
{
    ConfigurationHintLatch latch;
    // Nothing noted: a stray wake-up fetches nothing.
    assert(!HintCallsForConfiguration(latch.Take(), 9));

    // A rejoin brings the device's own ACTIVE update and the Provider's stamp,
    // both naming the revision fetched just before joining.
    latch.Note(Announced(9));
    latch.Note(Announced(9));
    ConfigurationHintLatch::Taken taken = latch.Take();
    assert(taken.pending && !taken.bare && taken.revision == 9);
    assert(!HintCallsForConfiguration(taken, 9));
    // Taking clears it.
    assert(!latch.Take().pending);

    // The Owner changed policy: a new revision is fetched.
    latch.Note(Announced(10));
    assert(HintCallsForConfiguration(latch.Take(), 9));
    // So is a lower one, after the Host's Authority was reset.
    latch.Note(Announced(2));
    assert(HintCallsForConfiguration(latch.Take(), 9));
}

void LatestAnnouncementWinsAndBareHintsDominate()
{
    ConfigurationHintLatch latch;
    latch.Note(Announced(10));
    latch.Note(Announced(11));
    ConfigurationHintLatch::Taken taken = latch.Take();
    assert(taken.revision == 11);
    assert(!HintCallsForConfiguration(taken, 11));
    assert(HintCallsForConfiguration(taken, 10));

    // An older Host's bare hint can only be answered by fetching, whatever
    // else was announced around it.
    latch.Note(Announced(11));
    latch.Note(PolicyAnnouncement{});
    latch.Note(Announced(11));
    taken = latch.Take();
    assert(taken.bare);
    assert(HintCallsForConfiguration(taken, 11));
    assert(!latch.Take().pending);
}

void NotesFromAnotherTaskAreNotLost()
{
    ConfigurationHintLatch latch;
    constexpr int kNotes = 20000;
    std::thread livekit([&] {
        for (int i = 1; i <= kNotes; ++i) latch.Note(Announced(static_cast<uint32_t>(i)));
    });
    uint32_t last = 0;
    int reads = 0;
    while (last != kNotes) {
        const ConfigurationHintLatch::Taken taken = latch.Take();
        if (!taken.pending) continue;
        ++reads;
        assert(!taken.bare && taken.revision > last);
        last = taken.revision;
    }
    livekit.join();
    assert(reads >= 1 && !latch.Take().pending);
}

}  // namespace

int main()
{
    ParsesOnlyPlainDecimal();
    FindsTheRevisionAmongOtherAttributes();
    RestatedRevisionIsNotFetched();
    LatestAnnouncementWinsAndBareHintsDominate();
    NotesFromAnotherTaskAreNotLost();
    std::puts("PASS: policy announcement parse, coalescing and refresh decision");
    return 0;
}
