#pragma once

#include <cstddef>

namespace eidolon {

inline constexpr char kOwnerTrustPartitionName[] = "owner_trust";
inline constexpr std::size_t kOwnerTrustPartitionBytes = 64 * 1024;

enum class OwnerTrustMigrationAction {
    UseDedicated,
    CopyLegacy,
    StartEmpty,
    RefuseUnavailable,
};

enum class OwnerTrustSourceState {
    Valid,
    Empty,
    Unavailable,
};

constexpr OwnerTrustMigrationAction ChooseOwnerTrustMigration(
    OwnerTrustSourceState dedicated,
    OwnerTrustSourceState legacy)
{
    if (dedicated == OwnerTrustSourceState::Valid) {
        return OwnerTrustMigrationAction::UseDedicated;
    }
    if (dedicated == OwnerTrustSourceState::Unavailable ||
        legacy == OwnerTrustSourceState::Unavailable) {
        return OwnerTrustMigrationAction::RefuseUnavailable;
    }
    return legacy == OwnerTrustSourceState::Valid
               ? OwnerTrustMigrationAction::CopyLegacy
               : OwnerTrustMigrationAction::StartEmpty;
}

}  // namespace eidolon
