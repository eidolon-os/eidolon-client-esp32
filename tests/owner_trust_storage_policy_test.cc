#include <cassert>

#include "eidolon/owner_trust_storage_policy.h"

int main()
{
    using eidolon::ChooseOwnerTrustMigration;
    using eidolon::OwnerTrustMigrationAction;
    using eidolon::OwnerTrustSourceState;

    assert(eidolon::kOwnerTrustPartitionBytes == 64 * 1024);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Valid,
                                     OwnerTrustSourceState::Valid) ==
           OwnerTrustMigrationAction::UseDedicated);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Valid,
                                     OwnerTrustSourceState::Unavailable) ==
           OwnerTrustMigrationAction::UseDedicated);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Empty,
                                     OwnerTrustSourceState::Valid) ==
           OwnerTrustMigrationAction::CopyLegacy);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Empty,
                                     OwnerTrustSourceState::Empty) ==
           OwnerTrustMigrationAction::StartEmpty);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Unavailable,
                                     OwnerTrustSourceState::Empty) ==
           OwnerTrustMigrationAction::RefuseUnavailable);
    assert(ChooseOwnerTrustMigration(OwnerTrustSourceState::Empty,
                                     OwnerTrustSourceState::Unavailable) ==
           OwnerTrustMigrationAction::RefuseUnavailable);
    return 0;
}
