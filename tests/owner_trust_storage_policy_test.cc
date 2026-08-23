#include <cassert>

#include "eidolon/owner_trust_storage_policy.h"

int main()
{
    using eidolon::ChooseOwnerTrustMigration;
    using eidolon::OwnerTrustMigrationAction;

    assert(eidolon::kOwnerTrustPartitionBytes == 64 * 1024);
    assert(ChooseOwnerTrustMigration(true, true) ==
           OwnerTrustMigrationAction::UseDedicated);
    assert(ChooseOwnerTrustMigration(true, false) ==
           OwnerTrustMigrationAction::UseDedicated);
    assert(ChooseOwnerTrustMigration(false, true) ==
           OwnerTrustMigrationAction::CopyLegacy);
    assert(ChooseOwnerTrustMigration(false, false) ==
           OwnerTrustMigrationAction::StartEmpty);
    return 0;
}
