#pragma once

#include <cstddef>

namespace eidolon {

inline constexpr char kOwnerTrustPartitionName[] = "owner_trust";
inline constexpr std::size_t kOwnerTrustPartitionBytes = 64 * 1024;

enum class OwnerTrustMigrationAction {
    UseDedicated,
    CopyLegacy,
    StartEmpty,
};

constexpr OwnerTrustMigrationAction ChooseOwnerTrustMigration(
    bool dedicated_bundle_valid,
    bool legacy_bundle_valid)
{
    if (dedicated_bundle_valid) {
        return OwnerTrustMigrationAction::UseDedicated;
    }
    return legacy_bundle_valid ? OwnerTrustMigrationAction::CopyLegacy
                               : OwnerTrustMigrationAction::StartEmpty;
}

}  // namespace eidolon
