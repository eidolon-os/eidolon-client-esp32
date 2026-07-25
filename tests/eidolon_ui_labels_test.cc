#include <cassert>
#include <cstring>

#include "eidolon/eidolon_ui_labels.h"

namespace {

void Expect(const char* actual, const char* expected)
{
    assert(actual != nullptr);
    assert(std::strcmp(actual, expected) == 0);
}

void ExpectStateAndDetail(const eidolon::EidolonUiSnapshot& snapshot,
                          const char* state, const char* detail)
{
    Expect(eidolon::CompactStateLabel(snapshot), state);
    Expect(eidolon::CompactVoiceDetail(snapshot), detail);
}

void TestLifecycleLabels()
{
    Expect(eidolon::EidolonBrandLabel(), "EIDOLON");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::Booting), "BOOT");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::Booting),
           "Starting Eidolon...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::LoadingAssets), "ASSETS");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::LoadingAssets),
           "Loading UI assets...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::WifiScanning), "WIFI");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::WifiScanning),
           "Scanning Wi-Fi...");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::WifiConnecting),
           "Connecting to Wi-Fi...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::WifiSetup), "SETUP");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::WifiSetup),
           "Wi-Fi setup mode");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::HubDiscovering), "HUB");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::HubDiscovering),
           "Finding Eidolon Hub...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::HubRegistering), "REGISTER");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::HubRegistering),
           "Registering device...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::Updating), "UPDATE");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::Updating),
           "Updating device...");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::Offline), "OFFLINE");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::Offline),
           "Check Wi-Fi and Hub");
    Expect(eidolon::LifecycleLabel(eidolon::LifecyclePhase::Error), "ERROR");
    Expect(eidolon::DefaultLifecycleDetail(eidolon::LifecyclePhase::Error),
           "Device setup failed");
}

void TestPairingTakesVisualPrecedence()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.connection = eidolon::ConnectionPhase::InRoom;
    snapshot.turn = eidolon::TurnPhase::AgentSpeaking;

    snapshot.pairing = eidolon::PairingStatus::PendingApproval;
    ExpectStateAndDetail(snapshot, "APPROVE", "Approve in Eidolon Admin");

    snapshot.pairing = eidolon::PairingStatus::WaitingBinding;
    ExpectStateAndDetail(snapshot, "BIND", "Bind an Agent in Admin");

    snapshot.pairing = eidolon::PairingStatus::Unauthorized;
    ExpectStateAndDetail(snapshot, "DENIED", "Approve this device again");
}

void TestConversationProjection()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.pairing = eidolon::PairingStatus::Active;
    snapshot.connection = eidolon::ConnectionPhase::InRoom;

    snapshot.turn = eidolon::TurnPhase::Idle;
    ExpectStateAndDetail(snapshot, "LISTEN", "Listening...");

    snapshot.turn = eidolon::TurnPhase::UserSpeaking;
    ExpectStateAndDetail(snapshot, "LISTEN", "Listening...");

    snapshot.turn = eidolon::TurnPhase::Recording;
    ExpectStateAndDetail(snapshot, "LISTEN", "Listening...");

    snapshot.turn = eidolon::TurnPhase::Committing;
    ExpectStateAndDetail(snapshot, "THINK", "Working on it...");

    snapshot.turn = eidolon::TurnPhase::AgentThinking;
    ExpectStateAndDetail(snapshot, "THINK", "Working on it...");

    snapshot.turn = eidolon::TurnPhase::AgentSpeaking;
    ExpectStateAndDetail(snapshot, "SPEAK", "Eidolon is speaking");
}

void TestConnectionProjection()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.pairing = eidolon::PairingStatus::Active;

    snapshot.connection = eidolon::ConnectionPhase::Connecting;
    ExpectStateAndDetail(snapshot, "JOINING", "Opening voice session...");

    snapshot.connection = eidolon::ConnectionPhase::Reconnecting;
    ExpectStateAndDetail(snapshot, "REJOIN", "Restoring connection...");

    snapshot.connection = eidolon::ConnectionPhase::Unreachable;
    ExpectStateAndDetail(snapshot, "OFFLINE", "Check Wi-Fi and Hub");

    snapshot.connection = eidolon::ConnectionPhase::Error;
    ExpectStateAndDetail(snapshot, "ERROR", "Voice session failed");

    snapshot.connection = eidolon::ConnectionPhase::Offline;
    ExpectStateAndDetail(snapshot, "OFFLINE", "Waiting for setup");
}

void TestConnectionOwnsTurnProjection()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.pairing = eidolon::PairingStatus::Active;
    // A delayed agent-phase packet must never keep the UI in a conversation
    // state after the transport has returned to the control room.
    snapshot.turn = eidolon::TurnPhase::AgentSpeaking;

    snapshot.connection = eidolon::ConnectionPhase::Connecting;
    Expect(eidolon::CompactStateLabel(snapshot), "JOINING");

    snapshot.connection = eidolon::ConnectionPhase::Ready;
    Expect(eidolon::CompactStateLabel(snapshot), "READY");
    Expect(eidolon::CompactVoiceDetail(snapshot), "Press BOOT to talk");
}

void TestReadyAndRecoveryDetails()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.pairing = eidolon::PairingStatus::Active;
    snapshot.connection = eidolon::ConnectionPhase::Ready;
    const eidolon::EndReason end_reasons[] = {
        eidolon::EndReason::None,
        eidolon::EndReason::IdleNormalEnd,
        eidolon::EndReason::ProactiveDone,
        eidolon::EndReason::UserLeft,
        eidolon::EndReason::Superseded,
        eidolon::EndReason::Error,
    };
    for (const auto end_reason : end_reasons) {
        snapshot.end_reason = end_reason;
        Expect(eidolon::CompactStateLabel(snapshot), "READY");
        Expect(eidolon::CompactVoiceDetail(snapshot), "Press BOOT to talk");
    }

    snapshot.connection = eidolon::ConnectionPhase::Reconnecting;
    ExpectStateAndDetail(snapshot, "REJOIN", "Restoring connection...");
}

void TestPresenceWakeProjectionAndReset()
{
    eidolon::EidolonUiSnapshot snapshot;
    snapshot.pairing = eidolon::PairingStatus::Active;
    snapshot.connection = eidolon::ConnectionPhase::Ready;
    snapshot.presence_wake = eidolon::PresenceWakePhase::VerifyingOwner;
    ExpectStateAndDetail(snapshot, "VERIFY", "Checking owner...");

    snapshot.presence_wake = eidolon::PresenceWakePhase::OwnerRecognized;
    ExpectStateAndDetail(snapshot, "OWNER", "Owner recognized");

    snapshot.connection = eidolon::ConnectionPhase::Connecting;
    ExpectStateAndDetail(snapshot, "JOINING", "Owner recognized");

    snapshot.presence_wake = eidolon::PresenceWakePhase::Idle;
    snapshot.connection = eidolon::ConnectionPhase::Ready;
    ExpectStateAndDetail(snapshot, "READY", "Press BOOT to talk");
}

}  // namespace

int main()
{
    TestLifecycleLabels();
    TestPairingTakesVisualPrecedence();
    TestConversationProjection();
    TestConnectionProjection();
    TestConnectionOwnsTurnProjection();
    TestReadyAndRecoveryDetails();
    TestPresenceWakeProjectionAndReset();
    return 0;
}
