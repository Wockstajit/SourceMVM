#include "../../AfxHookSource2/Filmmaker/Cosmetics/CosmeticThemeMath.h"
#include <cstdio>
#include <cstring>

namespace {
Filmmaker::CosmeticRecordedWeapon RecordedWeapon() {
    Filmmaker::CosmeticRecordedWeapon value;
    value.entity = 1234; value.handle = 0x123408; value.owner = 456; value.profileOwner = 789;
    value.definition = 17;
    value.itemHigh = 72; value.itemLow = 91; value.account = 456;
    value.fallbackPaint = 13;
    value.fallbackWear = Filmmaker::CosmeticRecordedWeapon::FloatBits(0.2f);
    value.fallbackSeed = 31; value.fallbackStat = ~0u;
    value.networked.Add(6, Filmmaker::CosmeticRecordedWeapon::FloatBits(1013.0f));
    value.networked.Add(7, 0x4444388e); // the demo's fractional seed, not the UI integer seed
    value.networked.Add(8, 0x80000000); // recorded signed zero must survive unchanged
    value.mesh = 2;
    return value;
}
}

int main() {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
    Filmmaker::CosmeticThemeAttributes saved;
    float seed = 744.9715f;
    uint32_t bits = 0;
    std::memcpy(&bits, &seed, sizeof(bits));
    CHECK(saved.Add(7, bits));
    CHECK(saved.Add(113, 123)); // stickers and other attributes are not owned
    CHECK(saved.present == 2);
    CHECK(!saved.Add(7, 0)); // duplicate must not overwrite the original
    CHECK(saved.bits[1] == bits);
    CHECK(saved.Add(6, 0x42300000));
    CHECK(saved.Add(8, 0x80000000)); // exact signed-zero bits survive
    CHECK(saved.Add(80, 0x7fc01234)); // storage never casts engine float bits
    CHECK(saved.Add(81, 0x12345678)); // score type is unrelated; the count is def80
    CHECK(saved.present == 15);
    for (int i = 0; i < 4; ++i)
        CHECK(Filmmaker::CosmeticThemeAttributes::Slot(Filmmaker::CosmeticThemeAttributes::Definition(i)) == i);
    CHECK(saved.bits[2] == 0x80000000);
    CHECK(saved.bits[3] == 0x7fc01234);
    Filmmaker::CosmeticRecordedWeapon weapon;
    weapon.entity = 1234; weapon.handle = 0x123408; weapon.owner = 456; weapon.definition = 17;
    CHECK(weapon.Matches(1234, 0x123408, 456, 17));
    CHECK(!weapon.Matches(1234, 0x223408, 456, 17)); // same index, new serial
    CHECK(!weapon.Matches(4321, 0x123408, 456, 17));
    CHECK(!weapon.Matches(1234, 0x123408, 654, 17));
    CHECK(!weapon.Matches(1234, 0x123408, 456, 7));
    using Filmmaker::CosmeticOwnedValue;
    using Filmmaker::CosmeticRecordedWeapon;
    using Filmmaker::CosmeticThemeAttributes;
    CHECK(CosmeticThemeAttributes::Encode(false, 0xffffffff) == 0);
    CHECK(CosmeticThemeAttributes::Encode(true, 0) == (uint64_t{1} << 32));
    CHECK(saved.Encoded(1) == (uint64_t{1} << 32 | bits));
    CHECK(saved.Encoded(2) == (uint64_t{1} << 32 | 0x80000000));
    auto membership = saved;
    membership.SetEncoded(1, 0);
    CHECK(membership.present == 13 && membership.Encoded(1) == 0);
    membership.SetEncoded(1, CosmeticThemeAttributes::Encode(true, 0));
    CHECK(membership.present == 15 && membership.bits[1] == 0);
    {
        CosmeticOwnedValue value;
        uint64_t original = 13;
        CHECK(!value.Owns(0)); // zero is a real owned value only when registered
        value.Attempt(115);
        CHECK(value.Owns(115));
        CHECK(!value.Observe(uint64_t{13}, original)); // failed A made no write
        CHECK(original == 13 && !value.haveAttempted && !value.haveConfirmed);
        value.Attempt(115);
        CHECK(!value.Observe(uint64_t{115}, original)); // false return with a partial write
        CHECK(original == 13 && value.confirmed == 115 && value.haveConfirmed);
        value.Attempt(653);
        CHECK(!value.Observe(uint64_t{115}, original)); // failed B, A survives
        CHECK(original == 13 && value.Owns(115) && !value.Owns(653));
        value.Attempt(653);
        CHECK(!value.Observe(uint64_t{653}, original));
        value.Attempt(44);
        CHECK(!value.Observe(uint64_t{653}, original)); // failed C, B survives
        CHECK(original == 13 && value.Owns(653));
        CHECK(value.Observe(uint64_t{1013}, original)); // authoritative packet supersedes A/B
        CHECK(original == 1013 && !value.haveConfirmed && !value.haveAttempted);
        value.Attempt(0);
        CHECK(value.Owns(0));
        CHECK(!value.Observe(uint64_t{0}, original));
        CHECK(original == 1013 && value.haveConfirmed && value.confirmed == 0);
        uint32_t exact = 0x80000000;
        value.Attempt(0);
        CHECK(!value.Observe(uint32_t{0}, exact));
        CHECK(exact == 0x80000000); // raw signed zero is never round-tripped through float
    }
    {
        // A vanilla pickup gains a local kill-eater count through the named setter.
        // A seek must not adopt that count as recorded data, and Reset must detect it.
        auto original = RecordedWeapon();
        original.networked = {};
        CosmeticRecordedWeapon journal;
        journal.Observe(original);
        journal.AttemptLocal(115, 0.02f, 42, 10, true);
        auto patched = original;
        patched.local.Add(80, CosmeticRecordedWeapon::FloatBits(10.0f));
        patched.local.Add(81, 0); // never owned or changed by a count override
        CHECK(!journal.Observe(patched));
        CHECK(journal.local.present == 0);
        CHECK(journal.localOwned[3].Owns(patched.local.Encoded(3)));
        CHECK(!journal.DataMatches(patched));
        journal.pending = true;
        CHECK(!journal.Observe(patched));
        CHECK(journal.local.present == 0 && !journal.DataMatches(patched));
        auto restored = patched;
        restored.local.SetEncoded(3, 0);
        CHECK(journal.DataMatches(restored));

        // An existing recorded counter keeps its exact bits; a new unowned value
        // from a packet can supersede the earlier baseline without losing other fields.
        original.networked.Add(80, 0x80000000);
        journal = {};
        journal.Observe(original);
        journal.AttemptNetwork(115, 0.02f, 42, 10);
        patched = original;
        patched.networked.bits[3] = CosmeticRecordedWeapon::FloatBits(10.0f);
        CHECK(!journal.Observe(patched));
        CHECK(journal.networked.bits[3] == 0x80000000);
        patched.networked.bits[3] = CosmeticRecordedWeapon::FloatBits(20.0f);
        CHECK(journal.Observe(patched));
        CHECK(journal.networked.bits[3] == patched.networked.bits[3]);
        CHECK(!journal.networkOwned[3].haveConfirmed);
    }
    const auto recorded = RecordedWeapon();
    {
        CosmeticRecordedWeapon journal;
        CHECK(journal.Observe(recorded));
        CHECK(journal.networked.bits[1] == 0x4444388e && journal.networked.bits[2] == 0x80000000);
        CHECK(journal.observedNetworkPresent == 7);
        CHECK(journal.observedItemHigh == recorded.itemHigh);
        journal.pending = journal.restored = journal.meshSelected = true;
        journal.dueFrame = 23; journal.lastCompositeFrame = 19;
        auto rawSnapshot = recorded;
        rawSnapshot.profileOwner = 0; // routing is maintained by the caller, not raw item enumeration
        CHECK(!journal.Observe(rawSnapshot));
        CHECK(journal.profileOwner == recorded.profileOwner);
        CHECK(journal.pending && journal.restored && journal.meshSelected);
        CHECK(journal.dueFrame == 23 && journal.lastCompositeFrame == 19);
        // Every full-identity component must retire ownership and previous restore progress.
        for (int field = 0; field < 4; ++field) {
            auto previous = journal;
            previous.rawOwned[3].Attempt(115);
            previous.networkOwned[0].Attempt(CosmeticThemeAttributes::Encode(true, 123));
            previous.localOwned[0].Attempt(CosmeticThemeAttributes::Encode(true, 456));
            previous.AttemptMesh(1);
            auto recreated = recorded;
            if (field == 0) recreated.entity = 4321;
            if (field == 1) recreated.handle += 0x10000; // index reused with a new serial
            if (field == 2) recreated.owner = 654;
            if (field == 3) recreated.definition = 7;
            recreated.fallbackPaint = 44; recreated.local.Add(6, 0x42f60000);
            // The input's bookkeeping must not be adopted, even if it came from a copied journal.
            recreated.pending = recreated.restored = recreated.meshSelected = true;
            recreated.dueFrame = recreated.lastCompositeFrame = 999;
            recreated.rawOwned[3].Attempt(653);
            CHECK(previous.Observe(recreated));
            CHECK(previous.Matches(recreated.entity, recreated.handle, recreated.owner, recreated.definition));
            CHECK(previous.fallbackPaint == 44 && previous.local.present == 1);
            CHECK(!previous.rawOwned[3].haveAttempted && !previous.networkOwned[0].haveAttempted);
            CHECK(!previous.localOwned[0].haveAttempted && !previous.meshOwned.haveAttempted);
            CHECK(!previous.meshScratchOwned.haveAttempted && !previous.meshScratchOwned.haveConfirmed);
            CHECK(!previous.pending && !previous.restored && !previous.meshSelected);
            CHECK(previous.dueFrame == 0 && previous.lastCompositeFrame == 0);
        }
    }
    {
        CosmeticRecordedWeapon journal;
        journal.Observe(recorded);
        journal.AttemptFallback(115, 0.37f, 991, -1, 12345, true);
        journal.AttemptNetwork(115, 0.37f, 991, -1);
        journal.AttemptLocal(115, 0.37f, 991, -1, false);
        journal.AttemptMesh(1);
        auto mixed = recorded;
        mixed.itemHigh = ~0u; mixed.itemLow = 0; mixed.account = 12345;
        mixed.fallbackPaint = 115;
        mixed.fallbackWear = CosmeticRecordedWeapon::FloatBits(0.19f); // new packet, partial clobber
        mixed.fallbackSeed = 991;
        mixed.networked.bits[0] = CosmeticRecordedWeapon::FloatBits(115.0f);
        mixed.networked.bits[1] = 0x44412345; // a genuinely new fractional recorded seed
        mixed.local.Add(6, CosmeticRecordedWeapon::FloatBits(115.0f));
        mixed.local.Add(7, CosmeticRecordedWeapon::FloatBits(991.0f));
        mixed.local.Add(8, CosmeticRecordedWeapon::FloatBits(0.37f));
        mixed.mesh = 1;
        CHECK(journal.Observe(mixed));
        CHECK(journal.itemHigh == 72 && journal.itemLow == 91 && journal.account == 456);
        CHECK(journal.observedItemHigh == ~0u);
        CHECK(journal.fallbackPaint == 13 && journal.fallbackSeed == 31);
        CHECK(journal.fallbackWear == mixed.fallbackWear);
        CHECK(journal.networked.bits[0] == recorded.networked.bits[0]);
        CHECK(journal.networked.bits[1] == 0x44412345 && journal.networked.bits[2] == 0x80000000);
        CHECK(journal.local.present == 0); // owned introduced attributes must be removed on rollback
        CHECK(journal.localOwned[0].haveConfirmed && journal.localOwned[1].haveConfirmed);
        CHECK(journal.mesh == 2 && journal.meshOwned.confirmed == 1);
        CHECK(!journal.rawOwned[4].haveConfirmed && journal.rawOwned[3].confirmed == 115);
        CHECK(!journal.networkOwned[1].haveConfirmed);
        // A second pick registers fresh values, retaining the original through a no-write failure.
        journal.AttemptFallback(653, 0.08f, 424, -1, 12345, true);
        journal.AttemptNetwork(653, 0.08f, 424, -1);
        journal.AttemptLocal(653, 0.08f, 424, -1, false);
        CHECK(!journal.Observe(mixed));
        CHECK(journal.fallbackPaint == 13 && journal.rawOwned[3].confirmed == 115);
        CHECK(journal.local.present == 0 && journal.localOwned[0].confirmed == mixed.local.Encoded(0));
        // B partially replaces A in only one list entry. Each owned component retains its own baseline.
        journal.AttemptLocal(653, 0.08f, 424, -1, false);
        mixed.local.bits[0] = CosmeticRecordedWeapon::FloatBits(653.0f);
        CHECK(!journal.Observe(mixed));
        CHECK(journal.local.present == 0 && journal.localOwned[0].confirmed == mixed.local.Encoded(0));
        CHECK(journal.localOwned[1].confirmed == mixed.local.Encoded(1));
        // A packet introduces a third, unowned value only in wear, then removes only seed.
        mixed.local.bits[2] = CosmeticRecordedWeapon::FloatBits(0.5f);
        mixed.local.SetEncoded(1, 0);
        CHECK(journal.Observe(mixed));
        CHECK(journal.local.present == 4 && journal.local.bits[2] == mixed.local.bits[2]);
        CHECK(!journal.localOwned[2].haveConfirmed && !journal.localOwned[1].haveConfirmed);
        CHECK(journal.localOwned[0].haveConfirmed);
    }
    {
        CosmeticRecordedWeapon journal;
        journal.Observe(recorded);
        journal.AttemptFallback(44, -0.0f, 19, -1, 999, false);
        CHECK(!journal.rawOwned[0].haveAttempted && !journal.rawOwned[1].haveAttempted);
        CHECK(!journal.rawOwned[2].haveAttempted);
        CHECK(journal.rawOwned[3].attempted == 44 && journal.rawOwned[4].attempted == 0x80000000);
        CHECK(journal.rawOwned[5].attempted == 19 && journal.rawOwned[6].attempted == ~0u);
        journal.rawOwned[0].Attempt(0); // fallback-id disabled while live high was -1
        CHECK(journal.rawOwned[0].Owns(0));
        journal.AttemptNetwork(44, -0.0f, 19, 33);
        CHECK(journal.networkOwned[0].haveAttempted && journal.networkOwned[1].haveAttempted);
        CHECK(journal.networkOwned[2].attempted == CosmeticThemeAttributes::Encode(true, 0x80000000));
        CHECK(!journal.networkOwned[3].haveAttempted); // network setter cannot add missing StatTrak
        CHECK(!journal.Observe(recorded)); // failed write must not create ownership or attributes
        CHECK(journal.local.present == 0 && !journal.rawOwned[3].haveConfirmed);
        auto changedNetwork = recorded;
        changedNetwork.networked.SetEncoded(0, 0);
        changedNetwork.networked.SetEncoded(3, CosmeticThemeAttributes::Encode(true, 0));
        CHECK(journal.Observe(changedNetwork));
        CHECK(journal.observedNetworkPresent == 14);
        journal.AttemptNetwork(44, 0.2f, 19, 33);
        CHECK(!journal.networkOwned[0].haveAttempted && journal.networkOwned[3].haveAttempted);
        CHECK(journal.networkOwned[3].attempted == CosmeticThemeAttributes::Encode(true,
            CosmeticRecordedWeapon::FloatBits(33.0f)));
        journal.Observe(changedNetwork);
        journal.AttemptNetwork(44, 0.2f, 19, -1);
        CHECK(!journal.networkOwned[3].haveAttempted); // existing recorded StatTrak remains untouched
        journal.AttemptLocal(44, 0.2f, 19, 33, false);
        CHECK(!journal.localOwned[3].haveAttempted);
        journal.AttemptLocal(44, 0.2f, 19, 33, true);
        CHECK(journal.localOwned[3].haveAttempted);
        CHECK(journal.localOwned[3].attempted == CosmeticThemeAttributes::Encode(true,
            CosmeticRecordedWeapon::FloatBits(33.0f)));
    }
    {
        CosmeticRecordedWeapon journal;
        journal.Observe(recorded);
        auto fallbackRecorded = recorded;
        fallbackRecorded.itemHigh = ~0u; // legitimate demo fallback, not necessarily a prior override
        journal.Observe(fallbackRecorded);
        CHECK(journal.itemHigh == ~0u && journal.observedItemHigh == ~0u);
        journal.rawOwned[0].Attempt(0);
        fallbackRecorded.itemHigh = 0;
        CHECK(!journal.Observe(fallbackRecorded));
        CHECK(journal.itemHigh == ~0u && journal.observedItemHigh == 0);
        CHECK(journal.rawOwned[0].haveConfirmed && journal.rawOwned[0].confirmed == 0);
    }
    {
        CosmeticRecordedWeapon journal;
        journal.Observe(recorded);
        journal.AttemptMesh(1);
        CHECK(!journal.Observe(recorded)); // field prearmed before a possibly partial renderer write
        CHECK(journal.mesh == 2 && journal.meshOwned.haveAttempted && journal.meshOwned.attempted == 1);
        CHECK(journal.meshScratchOwned.haveAttempted && journal.meshScratchOwned.attempted == 2);
        auto renderer = recorded;
        renderer.mesh = 1;
        CHECK(!journal.Observe(renderer));
        CHECK(journal.mesh == 2 && journal.meshOwned.haveConfirmed && !journal.meshOwned.haveAttempted);
        CHECK(!journal.meshScratchOwned.haveAttempted && !journal.meshScratchOwned.haveConfirmed);
        journal.AttemptMesh(3);
        CHECK(!journal.Observe(recorded)); // new prearm cannot discard A or pending B
        CHECK(journal.meshOwned.Owns(1) && journal.meshOwned.Owns(3));
        CHECK(!journal.Observe(renderer)); // B failed without replacing A
        CHECK(journal.meshOwned.Owns(1) && !journal.meshOwned.haveAttempted);
        journal.AttemptMesh(3);
        renderer.mesh = 3;
        CHECK(!journal.Observe(renderer)); // B partially wrote renderer
        CHECK(journal.mesh == 2 && journal.meshOwned.Owns(3));
        renderer.mesh = 4;
        CHECK(journal.Observe(renderer)); // unrelated recorded model/mesh update
        CHECK(journal.mesh == 4 && !journal.meshOwned.haveConfirmed && !journal.meshOwned.haveAttempted);
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
        CHECK(!journal.Observe(renderer));
    }
    {
        // After a seek, direct compositing can hold its target before native selection runs.
        // The next observation must not recapture that owned target as the recorded original.
        auto original = recorded;
        original.mesh = 1;
        CosmeticRecordedWeapon journal;
        journal.Observe(original);
        journal.AttemptMeshTarget(2);
        CHECK(!journal.Observe(original)); // baseline prearm does not consume the target intent
        CHECK(journal.meshOwned.Owns(2));
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
        auto target = original;
        target.mesh = 2;
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.haveConfirmed && journal.meshOwned.confirmed == 2);
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
        journal.pending = journal.restored = journal.meshSelected = true;
        CHECK(!journal.Observe(original)); // restore readback cannot retire the owed visual target
        CHECK(journal.restored && journal.meshSelected && journal.meshOwned.Owns(2));
        CHECK(!journal.Observe(target)); // delayed owned field write invalidates only mesh progress
        CHECK(journal.mesh == 1 && journal.restored && !journal.meshSelected);
    }
    {
        auto original = recorded;
        original.mesh = 1;
        CosmeticRecordedWeapon journal;
        journal.Observe(original);
        journal.AttemptMesh(5); // a partial native switch owns scratch=2 and target=5
        auto partial = original;
        partial.mesh = 2;
        CHECK(!journal.Observe(partial));
        journal.AttemptMeshTarget(5);
        CHECK(!journal.Observe(partial));
        CHECK(journal.mesh == 1 && journal.meshScratchOwned.Owns(2) && journal.meshOwned.Owns(5));
        partial.mesh = 5;
        CHECK(!journal.Observe(partial));
        CHECK(journal.mesh == 1 && journal.meshOwned.confirmed == 5);
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
        journal = {}; journal.Observe(original);
        journal.AttemptMeshTarget(1); // a direct same-mask hold must not fabricate scratch=2
        partial.mesh = 2;
        CHECK(journal.Observe(partial));
        CHECK(journal.mesh == 2 && !journal.meshOwned.haveConfirmed && !journal.meshOwned.haveAttempted);
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
    }
    {
        // Same-mask override and rollback both still use scratch=2; a partial call
        // must not promote that intermediate mask to the recorded original=1.
        auto original = recorded;
        original.mesh = 1;
        CosmeticRecordedWeapon journal;
        journal.Observe(original);
        journal.AttemptMesh(1);
        auto partial = original;
        partial.mesh = 2;
        CHECK(!journal.Observe(partial));
        CHECK(journal.mesh == 1 && journal.meshScratchOwned.Owns(2));
        CHECK(journal.meshOwned.haveAttempted && journal.meshOwned.attempted == 1);
        journal.AttemptMesh(journal.mesh); // failed rollback leaves scratch live again
        CHECK(!journal.Observe(partial));
        CHECK(journal.mesh == 1 && journal.meshScratchOwned.Owns(2));
        CHECK(journal.meshOwned.Owns(1));
        journal.AttemptMesh(journal.mesh); // next repair finally reaches original
        CHECK(!journal.Observe(original));
        CHECK(journal.mesh == 1);
        // A baseline field can also be prearmed, so ownership remains until runtime
        // confirms native renderer completion rather than inferring it from the field.
        CHECK(journal.meshOwned.Owns(1) && journal.meshScratchOwned.Owns(2));
        auto unrelated = original;
        unrelated.mesh = 4;
        CHECK(journal.Observe(unrelated));
        CHECK(journal.mesh == 4 && !journal.meshScratchOwned.haveConfirmed);
        CHECK(!journal.meshScratchOwned.haveAttempted && !journal.meshOwned.haveAttempted);
    }
    {
        // Original=1, pick=2 first prearms scratch=1, then confirms target=2.
        auto original = recorded;
        original.mesh = 1;
        CosmeticRecordedWeapon journal;
        journal.Observe(original);
        journal.AttemptMesh(2);
        CHECK(!journal.Observe(original));
        CHECK(journal.meshOwned.Owns(2) && journal.meshScratchOwned.Owns(1));
        auto target = original;
        target.mesh = 2;
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.haveConfirmed && journal.meshOwned.confirmed == 2);
        CHECK(!journal.meshScratchOwned.haveAttempted && !journal.meshScratchOwned.haveConfirmed);
        journal.AttemptMesh(1); // scratch=2 overlaps the previous confirmed target
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.Owns(2) && journal.meshOwned.Owns(1));
        CHECK(journal.meshOwned.haveAttempted && journal.meshOwned.attempted == 1);
        CHECK(journal.meshScratchOwned.haveConfirmed && journal.meshScratchOwned.confirmed == 2);
        CHECK(!journal.Observe(target)); // repeated scratch observation still cannot retire pending target=1
        CHECK(journal.meshOwned.Owns(2) && journal.meshOwned.haveAttempted && journal.meshOwned.attempted == 1);
        CHECK(journal.meshScratchOwned.Owns(2));
        journal.AttemptMesh(2); // target=2 now overlaps the previous confirmed scratch
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.haveConfirmed && journal.meshOwned.confirmed == 2);
        CHECK(!journal.meshOwned.haveAttempted && !journal.meshScratchOwned.haveConfirmed);
        CHECK(!journal.meshScratchOwned.haveAttempted);
        // Scratch failure must also preserve a previous confirmed target distinct
        // from baseline, scratch and the new target.
        journal.AttemptMesh(3);
        target.mesh = 3;
        CHECK(!journal.Observe(target));
        CHECK(journal.meshOwned.confirmed == 3);
        journal.AttemptMesh(5); // scratch=2, previous target=3, attempted target=5
        target.mesh = 2;
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.Owns(3) && journal.meshOwned.Owns(5));
        CHECK(journal.meshScratchOwned.Owns(2));
        target.mesh = 5;
        CHECK(!journal.Observe(target));
        CHECK(journal.mesh == 1 && journal.meshOwned.confirmed == 5 && !journal.meshOwned.haveAttempted);
        CHECK(!journal.meshScratchOwned.haveConfirmed && !journal.meshScratchOwned.haveAttempted);
    }
    {
        CosmeticRecordedWeapon journal;
        journal.Observe(recorded);
        journal.AttemptFallback(653, 0.08f, 424, -1, 12345, true);
        journal.AttemptLocal(653, 0.08f, 424, -1, false);
        journal.AttemptMesh(1);
        auto themed = recorded;
        themed.fallbackPaint = 653;
        themed.local.Add(6, CosmeticRecordedWeapon::FloatBits(653.0f));
        themed.mesh = 1;
        CHECK(!journal.Observe(themed));
        journal.pending = journal.restored = journal.meshSelected = true;
        CHECK(!journal.Observe(recorded)); // restored readback during the settling window
        CHECK(journal.DataMatches(recorded) && journal.restored && journal.meshSelected);
        CHECK(journal.rawOwned[3].Owns(653) && journal.localOwned[0].Owns(themed.local.Encoded(0)));
        CHECK(!journal.Observe(themed)); // delayed clobber stays owned, never becomes the original
        CHECK(journal.fallbackPaint == recorded.fallbackPaint && journal.local.present == recorded.local.present);
        CHECK(!journal.restored && !journal.meshSelected && !journal.DataMatches(themed));
        // A new non-owned recorded component supersedes only its original and invalidates progress.
        journal.restored = journal.meshSelected = true;
        themed.networked.bits[1] = 0x44412345;
        CHECK(journal.Observe(themed));
        CHECK(journal.networked.bits[1] == 0x44412345 && journal.fallbackPaint == recorded.fallbackPaint);
        CHECK(!journal.restored && !journal.meshSelected);
        // A rapid Clear before the first confirming read retains attempted writes too.
        journal = {}; journal.Observe(recorded);
        journal.AttemptFallback(653, 0.08f, 424, -1, 12345, true);
        journal.pending = journal.restored = true;
        CHECK(!journal.Observe(recorded));
        CHECK(journal.rawOwned[3].haveAttempted && journal.restored);
        auto late = recorded; late.fallbackPaint = 653;
        CHECK(!journal.Observe(late));
        CHECK(journal.fallbackPaint == recorded.fallbackPaint && !journal.restored);
        // A replacement effective pick returns to ordinary complete-observation semantics.
        journal.pending = false;
        CHECK(!journal.Observe(recorded));
        CHECK(!journal.rawOwned[3].haveConfirmed && !journal.rawOwned[3].haveAttempted);
    }
    using Filmmaker::CosmeticThemePawn;
    Filmmaker::CosmeticThemePawnIdentity identity;
    identity.pawn = 1234; identity.pawnHandle = 0x123408; identity.controllerHandle = 0x112208;
    identity.steamId = 456; identity.team = 2;
    CHECK(identity.Valid());
    auto changed = identity;
    changed.pawn = 4321; CHECK(!identity.Matches(changed));
    changed = identity; changed.pawnHandle += 0x10000; CHECK(!identity.Matches(changed));
    changed = identity; changed.controllerHandle += 0x10000; CHECK(!identity.Matches(changed));
    changed = identity; changed.steamId = 654; CHECK(!identity.Matches(changed));
    changed = identity; changed.team = 3; CHECK(!identity.Matches(changed));
    changed = identity; changed.steamId = 0; CHECK(changed.Valid()); // bot identity is handle-bound
    changed.team = 0; CHECK(!changed.Valid());
    changed = identity; changed.pawnHandle = ~0u; CHECK(!changed.Valid());
    changed = identity; changed.controllerHandle = ~0u; CHECK(!changed.Valid());
    changed = identity; changed.pawn = 0; CHECK(!changed.Valid());
    CosmeticThemePawn model;
    CHECK(model.Observe(identity, "recorded.vmdl") == CosmeticThemePawn::Captured);
    model.attempted = "themeA.vmdl";
    CHECK(model.Observe(identity, "themeA.vmdl") == CosmeticThemePawn::Retained);
    CHECK(model.original == "recorded.vmdl" && model.confirmed == "themeA.vmdl" && model.attempted.empty());
    model.attempted = "themeB.vmdl";
    CHECK(model.Observe(identity, "themeA.vmdl") == CosmeticThemePawn::Retained); // failed B made no write
    CHECK(model.original == "recorded.vmdl" && model.confirmed == "themeA.vmdl");
    model.attempted = "themeB.vmdl";
    CHECK(model.Observe(identity, "themeB.vmdl") == CosmeticThemePawn::Retained); // failed B partially wrote
    model.attempted = "themeC.vmdl";
    CHECK(model.Owns("THEMEB.VMDL") && model.Owns("themeC.vmdl"));
    const auto before = model;
    CHECK(model.Observe(identity, "") == CosmeticThemePawn::Refused);
    CHECK(model.Observe(identity, nullptr) == CosmeticThemePawn::Refused);
    CHECK(model.Observe(changed, "themeC.vmdl") == CosmeticThemePawn::Refused);
    CHECK(model.original == before.original && model.confirmed == before.confirmed
        && model.attempted == before.attempted && model.identity.Matches(before.identity));
    CHECK(model.Observe(identity, "themeB.vmdl") == CosmeticThemePawn::Retained);
    CHECK(model.original == "recorded.vmdl"); // C failed without a write; B remains owned
    CHECK(model.Observe(identity, "updated-recorded.vmdl") == CosmeticThemePawn::Refreshed);
    CHECK(model.original == "updated-recorded.vmdl" && !model.Owns("themeB.vmdl"));
    model.attempted = "themeD.vmdl";
    changed = identity; changed.pawnHandle += 0x10000;
    CHECK(model.Observe(changed, "new-pawn.vmdl") == CosmeticThemePawn::Captured);
    CHECK(model.original == "new-pawn.vmdl" && model.confirmed.empty() && model.attempted.empty());
    CHECK(model.identity.Matches(changed));
    model.attempted = "themeE.vmdl";
    CHECK(model.NeedsRestore("themeE.vmdl"));
    CHECK(!model.NeedsRestore("new-pawn.vmdl")); // recorded data already restored before any native attempt
    model.restoring = true;
    CHECK(model.NeedsRestore("new-pawn.vmdl")); // partial restore must still pay a successful render refresh
    CHECK(!model.NeedsRestore("superseding-recorded.vmdl"));
    return failures ? 1 : 0;
}
