#include "eidolon_ui_labels.h"

namespace eidolon {

const char* EidolonBrandLabel()
{
    return "EIDOLON";
}

const char* UiSceneLabel(UiScene scene)
{
    switch (scene) {
    case UiScene::Starting:
        return "BOOT";
    case UiScene::Loading:
        return "ASSETS";
    case UiScene::Network:
        return "NETWORK";
    case UiScene::Commissioning:
        return "SETUP";
    case UiScene::Updating:
        return "UPDATE";
    case UiScene::WaitingApproval:
        return "REVIEW";
    case UiScene::PreparingService:
        return "SERVICE";
    case UiScene::Ready:
        return "READY";
    case UiScene::OpeningConversation:
        return "OPENING";
    case UiScene::Conversation:
        return "LISTEN";
    case UiScene::Reconnecting:
        return "REJOIN";
    case UiScene::Ended:
        return "ENDED";
    case UiScene::Removed:
        return "REMOVED";
    case UiScene::RecoveryRequired:
        return "RESET";
    case UiScene::Error:
    default:
        return "ERROR";
    }
}

const char* UiSceneStatus(UiScene scene)
{
    switch (scene) {
    case UiScene::Starting:
        return "Starting Eidolon";
    case UiScene::Loading:
        return "Loading assets";
    case UiScene::Network:
        return "Connecting network";
    case UiScene::Commissioning:
        return "Device setup";
    case UiScene::Updating:
        return "Updating device";
    case UiScene::WaitingApproval:
        return "Waiting for approval";
    case UiScene::PreparingService:
        return "Preparing service";
    case UiScene::Ready:
        return "Ready";
    case UiScene::OpeningConversation:
        return "Opening conversation";
    case UiScene::Conversation:
        return "Listening";
    case UiScene::Reconnecting:
        return "Reconnecting";
    case UiScene::Ended:
        return "Conversation ended";
    case UiScene::Removed:
        return "Device removed";
    case UiScene::RecoveryRequired:
        return "Recovery required";
    case UiScene::Error:
    default:
        return "Unavailable";
    }
}

const char* UiSceneDetail(UiScene scene, EndReason end_reason)
{
    switch (scene) {
    case UiScene::Starting:
        return "Starting Eidolon...";
    case UiScene::Loading:
        return "Loading UI assets...";
    case UiScene::Network:
        return "Connecting to Owner network...";
    case UiScene::Commissioning:
        return "Complete setup on your controller";
    case UiScene::Updating:
        return "Updating device...";
    case UiScene::WaitingApproval:
        return "Approve this device in Eidolon";
    case UiScene::PreparingService:
        return "Device claimed; service is not ready";
    case UiScene::Ready:
        return "Start a conversation";
    case UiScene::OpeningConversation:
        return "Waiting for Channel confirmation...";
    case UiScene::Conversation:
        return "Listening...";
    case UiScene::Reconnecting:
        return "Restoring Channel connection...";
    case UiScene::Ended:
        return end_reason == EndReason::Error ? "Conversation ended with an error"
                                              : "Ready for another conversation";
    case UiScene::Removed:
        return "Voice service disabled; reset is required";
    case UiScene::RecoveryRequired:
        return "Use the physical recovery procedure";
    case UiScene::Error:
    default:
        return "Service unavailable";
    }
}

const char* UiSceneEmotion(UiScene scene)
{
    switch (scene) {
    case UiScene::WaitingApproval:
    case UiScene::PreparingService:
    case UiScene::OpeningConversation:
    case UiScene::Reconnecting:
        return "thinking";
    case UiScene::Ended:
        return "happy";
    case UiScene::Removed:
    case UiScene::RecoveryRequired:
    case UiScene::Error:
        return "sad";
    default:
        return "neutral";
    }
}

}  // namespace eidolon
