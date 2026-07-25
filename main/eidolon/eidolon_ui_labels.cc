#include "eidolon_ui_labels.h"

namespace eidolon {

const char* EidolonBrandLabel()
{
    return "EIDOLON";
}

const char* LifecycleLabel(LifecyclePhase phase)
{
    switch (phase) {
    case LifecyclePhase::Booting:
        return "BOOT";
    case LifecyclePhase::LoadingAssets:
        return "ASSETS";
    case LifecyclePhase::WifiScanning:
    case LifecyclePhase::WifiConnecting:
        return "WIFI";
    case LifecyclePhase::WifiSetup:
        return "SETUP";
    case LifecyclePhase::HubDiscovering:
        return "HUB";
    case LifecyclePhase::HubRegistering:
        return "REGISTER";
    case LifecyclePhase::Updating:
        return "UPDATE";
    case LifecyclePhase::Offline:
        return "OFFLINE";
    case LifecyclePhase::Error:
        return "ERROR";
    case LifecyclePhase::Operational:
    default:
        return "READY";
    }
}

const char* DefaultLifecycleDetail(LifecyclePhase phase)
{
    switch (phase) {
    case LifecyclePhase::Booting:
        return "Starting Eidolon...";
    case LifecyclePhase::LoadingAssets:
        return "Loading UI assets...";
    case LifecyclePhase::WifiScanning:
        return "Scanning Wi-Fi...";
    case LifecyclePhase::WifiConnecting:
        return "Connecting to Wi-Fi...";
    case LifecyclePhase::WifiSetup:
        return "Wi-Fi setup mode";
    case LifecyclePhase::HubDiscovering:
        return "Finding Eidolon Hub...";
    case LifecyclePhase::HubRegistering:
        return "Registering device...";
    case LifecyclePhase::Updating:
        return "Updating device...";
    case LifecyclePhase::Offline:
        return "Check Wi-Fi and Hub";
    case LifecyclePhase::Error:
        return "Device setup failed";
    case LifecyclePhase::Operational:
    default:
        return "";
    }
}

const char* CompactStateLabel(const EidolonUiSnapshot& snapshot)
{
    // Pairing/authorization always wins: READY while approval is still required
    // is actively misleading, regardless of the underlying connection phase.
    switch (snapshot.pairing) {
    case PairingStatus::PendingApproval:
        return "APPROVE";
    case PairingStatus::WaitingBinding:
        return "BIND";
    case PairingStatus::Unauthorized:
        return "DENIED";
    case PairingStatus::Active:
    default:
        break;
    }

    if (snapshot.presence_wake == PresenceWakePhase::VerifyingOwner &&
        snapshot.connection != ConnectionPhase::InRoom) {
        return "VERIFY";
    }
    if (snapshot.presence_wake == PresenceWakePhase::OwnerRecognized &&
        snapshot.connection != ConnectionPhase::Connecting &&
        snapshot.connection != ConnectionPhase::Reconnecting &&
        snapshot.connection != ConnectionPhase::InRoom) {
        return "OWNER";
    }

    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
        return "JOINING";
    case ConnectionPhase::Reconnecting:
        return "REJOIN";
    case ConnectionPhase::Unreachable:
        return "OFFLINE";
    case ConnectionPhase::Error:
        return "ERROR";
    case ConnectionPhase::InRoom:
        switch (snapshot.turn) {
        case TurnPhase::Committing:
        case TurnPhase::AgentThinking:
            return "THINK";
        case TurnPhase::AgentSpeaking:
            return "SPEAK";
        case TurnPhase::Recording:
        case TurnPhase::UserSpeaking:
        case TurnPhase::Idle:
        default:
            return "LISTEN";
        }
    case ConnectionPhase::Ready:
        return "READY";
    case ConnectionPhase::Offline:
    default:
        return "OFFLINE";
    }
}

const char* CompactVoiceDetail(const EidolonUiSnapshot& snapshot)
{
    switch (snapshot.pairing) {
    case PairingStatus::PendingApproval:
        return "Approve in Eidolon Admin";
    case PairingStatus::WaitingBinding:
        return "Bind an Agent in Admin";
    case PairingStatus::Unauthorized:
        return "Approve this device again";
    case PairingStatus::Active:
    default:
        break;
    }

    if (snapshot.presence_wake == PresenceWakePhase::VerifyingOwner &&
        snapshot.connection != ConnectionPhase::InRoom) {
        return "Checking owner...";
    }
    if (snapshot.presence_wake == PresenceWakePhase::OwnerRecognized &&
        snapshot.connection != ConnectionPhase::InRoom) {
        return "Owner recognized";
    }

    switch (snapshot.connection) {
    case ConnectionPhase::Connecting:
        return "Opening voice session...";
    case ConnectionPhase::Reconnecting:
        return "Restoring connection...";
    case ConnectionPhase::Unreachable:
        return "Check Wi-Fi and Hub";
    case ConnectionPhase::Error:
        return "Voice session failed";
    case ConnectionPhase::InRoom:
        switch (snapshot.turn) {
        case TurnPhase::Committing:
        case TurnPhase::AgentThinking:
            return "Working on it...";
        case TurnPhase::AgentSpeaking:
            return "Eidolon is speaking";
        case TurnPhase::Recording:
        case TurnPhase::UserSpeaking:
        case TurnPhase::Idle:
        default:
            return "Listening...";
        }
    case ConnectionPhase::Ready:
        // READY describes the current actionable state, not how the previous
        // session ended. Once the transport has settled back on the control
        // room, the next action is identical to first boot.
        return "Press BOOT to talk";
    case ConnectionPhase::Offline:
    default:
        return "Waiting for setup";
    }
}

}  // namespace eidolon
