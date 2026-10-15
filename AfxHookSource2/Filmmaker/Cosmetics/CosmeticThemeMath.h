#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace Filmmaker {

// Keep the recorded float bits, including fractional paint seeds. Converting through
// the UI's integer seed representation would change the restored demo attributes.
struct CosmeticThemeAttributes {
    uint32_t bits[4] = {};
    unsigned present = 0;
    static int Slot(unsigned def) {
        return def == 6 ? 0 : def == 7 ? 1 : def == 8 ? 2 : def == 80 ? 3 : -1;
    }
    static unsigned Definition(int slot) {
        const unsigned defs[] = {6, 7, 8, 80};
        return defs[slot];
    }
    static uint64_t Encode(bool isPresent, uint32_t value) {
        return isPresent ? (uint64_t{1} << 32) | value : 0;
    }
    uint64_t Encoded(unsigned slot) const {
        return slot < 4 ? Encode((present & (1u << slot)) != 0, bits[slot]) : 0;
    }
    void SetEncoded(unsigned slot, uint64_t value) {
        if (slot >= 4) return;
        const unsigned flag = 1u << slot;
        if (value & (uint64_t{1} << 32)) {
            present |= flag;
            bits[slot] = static_cast<uint32_t>(value);
        } else {
            present &= ~flag;
            bits[slot] = 0;
        }
    }
    bool Add(unsigned def, uint32_t value) {
        const int slot = Slot(def);
        if (slot < 0) return true; // unrelated attributes belong to the engine
        const unsigned flag = 1u << slot;
        if (present & flag) return false; // ambiguous duplicate: refuse the snapshot
        present |= flag;
        bits[slot] = value;
        return true;
    }
};

// A seek may leave the same live pawn dressed by us. Full serial-bearing handles,
// reciprocal bindings, SteamID and team distinguish that survivor from a recreated pawn.
struct CosmeticThemePawnIdentity {
    uintptr_t pawn = 0;
    uint32_t pawnHandle = ~0u, controllerHandle = ~0u;
    uint64_t steamId = 0;
    int team = 0;
    bool Valid() const {
        return pawn && pawnHandle != ~0u && controllerHandle != ~0u && (team == 2 || team == 3);
    }
    bool Matches(const CosmeticThemePawnIdentity& other) const {
        return pawn == other.pawn && pawnHandle == other.pawnHandle
            && controllerHandle == other.controllerHandle && steamId == other.steamId && team == other.team;
    }
};

inline bool CosmeticThemeModelEqual(const std::string& a, const char* b) {
    if (!b || a.size() != std::char_traits<char>::length(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + ('a' - 'A') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + ('a' - 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}

struct CosmeticThemePawn {
    CosmeticThemePawnIdentity identity;
    std::string original, confirmed, attempted;
    bool pending = false, restoring = false;
    uint64_t dueFrame = 0;
    enum Observation { Refused, Captured, Retained, Refreshed };
    bool Owns(const char* live) const {
        return (live && *live) && ((!confirmed.empty() && CosmeticThemeModelEqual(confirmed, live))
            || (!attempted.empty() && CosmeticThemeModelEqual(attempted, live)));
    }
    bool NeedsRestore(const char* live) const {
        return Owns(live) || (restoring && CosmeticThemeModelEqual(original, live));
    }
    Observation Observe(const CosmeticThemePawnIdentity& current, const char* live) {
        // Failed reads must neither replace an existing baseline nor permit a native write.
        if (!current.Valid() || !live || !*live) return Refused;
        Observation result;
        if (!identity.Matches(current) || original.empty()) {
            identity = current;
            original = live;
            confirmed.clear();
            result = Captured;
        } else if (Owns(live)) {
            // Resolve a previous call, including a false return after a partial native write.
            confirmed = live;
            result = Retained;
        } else {
            original = live; // a new authoritative recorded model supersedes the old baseline
            confirmed.clear();
            result = Refreshed;
        }
        attempted.clear();
        return result;
    }
};

// Register intent before touching engine memory. A failed native call may still have
// written some fields; the next complete observation resolves that intent per component.
struct CosmeticOwnedValue {
    uint64_t confirmed = 0, attempted = 0;
    bool haveConfirmed = false, haveAttempted = false;

    bool Owns(uint64_t value) const {
        return (haveConfirmed && confirmed == value) || (haveAttempted && attempted == value);
    }
    void Attempt(uint64_t value) {
        attempted = value;
        haveAttempted = true;
    }
    bool Observe(uint64_t current, uint64_t& baseline) {
        bool changed = false;
        if (Owns(current)) {
            confirmed = current;
            haveConfirmed = true;
        } else {
            changed = current != baseline;
            baseline = current;
            confirmed = 0;
            haveConfirmed = false;
        }
        attempted = 0;
        haveAttempted = false;
        return changed;
    }
    bool Observe(uint32_t current, uint32_t& baseline) {
        uint64_t wide = baseline;
        const bool changed = Observe(static_cast<uint64_t>(current), wide);
        baseline = static_cast<uint32_t>(wide);
        return changed;
    }
};

struct CosmeticRecordedWeapon {
    uintptr_t entity = 0;
    uint32_t handle = 0;
    uint64_t owner = 0, profileOwner = 0;
    int definition = 0;
    uint32_t itemHigh = 0, itemLow = 0, account = 0;
    uint32_t fallbackPaint = 0, fallbackWear = 0, fallbackSeed = 0, fallbackStat = 0;
    CosmeticThemeAttributes networked, local;
    uint64_t mesh = 0, dueFrame = 0;
    bool pending = false, restored = false, meshSelected = false;
    CosmeticOwnedValue rawOwned[7], networkOwned[4], localOwned[4], meshOwned, meshScratchOwned;
    unsigned observedNetworkPresent = 0;
    uint32_t observedItemHigh = 0;
    uint64_t lastCompositeFrame = 0;

    bool Matches(uintptr_t pointer, uint32_t fullHandle, uint64_t xuid, int def) const {
        return entity == pointer && handle == fullHandle && owner == xuid && definition == def;
    }
    uint32_t& Raw(unsigned slot) {
        switch (slot) {
        case 0: return itemHigh;
        case 1: return itemLow;
        case 2: return account;
        case 3: return fallbackPaint;
        case 4: return fallbackWear;
        case 5: return fallbackSeed;
        default: return fallbackStat;
        }
    }
    uint32_t Raw(unsigned slot) const {
        switch (slot) {
        case 0: return itemHigh;
        case 1: return itemLow;
        case 2: return account;
        case 3: return fallbackPaint;
        case 4: return fallbackWear;
        case 5: return fallbackSeed;
        default: return fallbackStat;
        }
    }
    static uint32_t FloatBits(float value) {
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(value), "32-bit engine attributes required");
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
    bool Observe(const CosmeticRecordedWeapon& live) {
        if (!entity || !Matches(live.entity, live.handle, live.owner, live.definition)) {
            *this = live;
            for (auto& value : rawOwned) value = {};
            for (auto& value : networkOwned) value = {};
            for (auto& value : localOwned) value = {};
            meshOwned = {};
            meshScratchOwned = {};
            observedNetworkPresent = live.networked.present;
            observedItemHigh = live.itemHigh;
            pending = restored = meshSelected = false;
            dueFrame = lastCompositeFrame = 0;
            return true;
        }
        // Packet updates can replace only some raw fields or list entries. A whole-item
        // equality check would either recapture our writes or miss new recorded values.
        bool changed = false;
        for (unsigned slot = 0; slot < 7; ++slot)
            if (!pending || live.Raw(slot) != Raw(slot))
                changed = rawOwned[slot].Observe(live.Raw(slot), Raw(slot)) || changed;
        for (unsigned slot = 0; slot < 4; ++slot) {
            uint64_t recorded = networked.Encoded(slot);
            if (!pending || live.networked.Encoded(slot) != recorded)
                changed = networkOwned[slot].Observe(live.networked.Encoded(slot), recorded) || changed;
            networked.SetEncoded(slot, recorded);
            recorded = local.Encoded(slot);
            if (!pending || live.local.Encoded(slot) != recorded)
                changed = localOwned[slot].Observe(live.local.Encoded(slot), recorded) || changed;
            local.SetEncoded(slot, recorded);
        }
        observedNetworkPresent = live.networked.present;
        observedItemHigh = live.itemHigh;
        if (live.mesh != mesh) {
            const bool attemptedTarget = meshOwned.haveAttempted && meshOwned.attempted == live.mesh;
            // A new attempt's scratch may equal the previous confirmed target. Retain
            // both target obligations until the setter's second call completes. Conversely,
            // a new attempted target can supersede an old confirmed scratch value.
            if (attemptedTarget || (!meshScratchOwned.Owns(live.mesh) && meshOwned.Owns(live.mesh))) {
                changed = meshOwned.Observe(live.mesh, mesh) || changed;
                meshScratchOwned = {};
            } else if (meshScratchOwned.Owns(live.mesh)) {
                changed = meshScratchOwned.Observe(live.mesh, mesh) || changed;
            } else {
                changed = meshOwned.Observe(live.mesh, mesh) || changed;
                meshScratchOwned = {};
            }
        }
        // Compositing deliberately prearms the recorded field while the renderer owns
        // a different mask. Seeing that baseline cannot cancel either owned obligation.
        // Pending rollback may see its own restored values before a late engine clobber;
        // retain ownership until visual completion, and never trust stale progress flags.
        if (pending) {
            if (changed || !DataMatches(live)) restored = false;
            if (changed || live.mesh != mesh) meshSelected = false;
        }
        return changed;
    }
    bool DataMatches(const CosmeticRecordedWeapon& live) const {
        for (unsigned slot = 0; slot < 7; ++slot) if (Raw(slot) != live.Raw(slot)) return false;
        for (unsigned slot = 0; slot < 4; ++slot)
            if (networked.Encoded(slot) != live.networked.Encoded(slot)
                || local.Encoded(slot) != live.local.Encoded(slot)) return false;
        return true;
    }
    void AttemptMeshTarget(uint64_t mask) {
        // A direct field hold writes only the target; retain any earlier native scratch intent.
        meshOwned.Attempt(mask);
    }
    void AttemptMesh(uint64_t mask) {
        // The native setter first toggles to a scratch mask, then writes the target.
        // Either call can partially fail, including a restore whose target equals baseline.
        meshScratchOwned.Attempt((mask & 1) ? 2 : 1);
        meshOwned.Attempt(mask);
    }
    void AttemptFallback(int paint, float wear, int seed, int stat, uint32_t accountId, bool fallback) {
        rawOwned[3].Attempt(static_cast<uint32_t>(paint));
        rawOwned[4].Attempt(FloatBits(wear));
        rawOwned[5].Attempt(static_cast<uint32_t>(seed));
        rawOwned[6].Attempt(static_cast<uint32_t>(stat));
        if (fallback) {
            rawOwned[0].Attempt(~0u);
            rawOwned[1].Attempt(0);
            rawOwned[2].Attempt(accountId);
        }
    }
    void AttemptNetwork(int paint, float wear, int seed, int stat) {
        const uint32_t values[] = {FloatBits(static_cast<float>(paint)), FloatBits(static_cast<float>(seed)),
            FloatBits(wear), FloatBits(static_cast<float>(stat))};
        for (unsigned slot = 0; slot < 4; ++slot)
            if ((observedNetworkPresent & (1u << slot)) && (slot != 3 || stat >= 0))
                networkOwned[slot].Attempt(CosmeticThemeAttributes::Encode(true, values[slot]));
    }
    void AttemptLocal(int paint, float wear, int seed, int stat, bool includeStat) {
        const uint32_t values[] = {FloatBits(static_cast<float>(paint)), FloatBits(static_cast<float>(seed)),
            FloatBits(wear), FloatBits(static_cast<float>(stat))};
        for (unsigned slot = 0; slot < 4; ++slot)
            if (slot != 3 || (includeStat && stat >= 0))
                localOwned[slot].Attempt(CosmeticThemeAttributes::Encode(true, values[slot]));
    }
};

} // namespace Filmmaker
