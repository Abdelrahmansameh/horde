// The per-family hit-flash table. See HitFlash.h for why it is a table, and
// why it sits in sim/ rather than beside vfx/DeathVfx.h.
#include "sim/chaff/HitFlash.h"

namespace immune::sim {
namespace {

/// The shipped looks. BOTH FAMILIES FLASH THE SAME WAY, deliberately.
///
/// A death burst is an identity statement — it is the last thing an enemy does
/// and it gets to be family-specific (vfx/DeathVfx.cpp spends real variety on
/// it). A hit flash is the opposite kind of signal: it says "this landed", it
/// has to be read peripherally while the player is looking somewhere else, and
/// it fires thousands of times a wave. A signal like that wants ONE grammar, so
/// that a flash always means the same thing no matter what it is on.
///
/// So the table ships uniform, and exists anyway — because the moment a third
/// family arrives that should read as armoured (low gain, so only real chunks
/// register) or as fragile (high gain, lighting up under any pressure), the
/// place to say so is already here and already in the file.
struct FlashTables {
    HitFlashParams look[kFamilyCount];
};

FlashTables& tables() {
    static FlashTables t;
    return t;
}

u32 family_slot(PathogenFamily family) {
    const u32 i = static_cast<u32>(family);
    return i < kFamilyCount ? i : 0u;
}

} // namespace

const HitFlashParams& family_hit_flash(PathogenFamily family) {
    return tables().look[family_slot(family)];
}

void set_family_hit_flash(PathogenFamily family, const HitFlashParams& params) {
    tables().look[family_slot(family)] = params;
}

} // namespace immune::sim
