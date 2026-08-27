// The per-family death-burst table. See DeathVfx.h for why it is a table.
#include "vfx/DeathVfx.h"

namespace immune::vfx {
namespace {

/// The shipped looks. Two families, two readings of the same event, because the
/// horde is fought as a mass and "what did I just kill" has to survive a glance
/// at 60 fps.
///
/// BOTH ARE EXPLOSIONS. Both throw two dozen-odd hard-edged fragments at 10-30
/// units a second against a heavy drag, and both are finished inside a third of
/// a second. That shared grammar is the point: a death is a small violent
/// event, and the family read has to happen INSIDE it rather than by making one
/// family's death a slower, softer, different kind of thing.
///
///   VIRUS — a capsid detonating. Ordered (bit_scatter 0.15, so the fragments
///   leave on near-even spokes like a shell failing all at once), fast-spinning,
///   the smallest pieces, the whitest core, and the shortest life of the two.
///   It is the common case and it has to stay cheap on the eye.
///
///   BACTERIA — a bigger body blowing apart. More fragments and chunkier ones,
///   thrown at RANDOM angles (bit_scatter 0.90) with barely any tumble, sagging
///   twice as hard, and lasting a few frames longer. Bigger prize, bigger mess,
///   still an explosion.
///
/// The colours here are the DESIGN.md 6 family codes and duplicate
/// render::FamilyTables. That duplication is deliberate and bounded:
/// game::default_game_config() overwrites `color` from render::family_color()
/// when it bootstraps enemies.json, so the shipped file can never disagree with
/// the body colour, and these values only ever surface in a vfx-only test that
/// loaded no config at all.
struct DeathTables {
    FamilyDeathVfx look[kFamilyCount];

    DeathTables() {
        FamilyDeathVfx& virus = look[static_cast<u32>(PathogenFamily::Virus)];
        virus.style = DeathStyle::Burst;
        virus.color = Vec4{0.20f, 0.94f, 0.38f, 1.0f};
        // Everything else is already the struct's default, which is authored as
        // the Virus look precisely because the Virus is the common case.

        FamilyDeathVfx& bacteria = look[static_cast<u32>(PathogenFamily::Bacteria)];
        // Shards, not droplets. The Bacteria shipped on DeathStyle::Lyse first
        // and read as a blurry smudge: a Tracer is a soft glow, and eight of
        // them landing on one corpse composite into a single fuzzy blob instead
        // of into pieces. Whatever a cell wall does biologically, on screen it
        // has to come apart into things with edges.
        bacteria.style = DeathStyle::Burst;
        bacteria.color = Vec4{0.72f, 0.80f, 0.22f, 1.0f};

        bacteria.scale = 1.20f;

        bacteria.core_size = 1.00f;
        bacteria.core_life = 0.08f;
        bacteria.core_white = 0.35f;

        bacteria.ring_size = 1.60f;
        bacteria.ring_life = 0.12f;
        bacteria.ring_alpha = 0.22f;

        // A few more pieces, marginally larger. The gap is small on purpose:
        // sizes are multiples of the agent radius and a bacterium's is 1.5x a
        // virus's, so these numbers being nearly equal already means the
        // bacterium's fragments come out half again as big on screen.
        bacteria.bit_count = 28;
        bacteria.bit_size_min = 0.22f;
        bacteria.bit_size_max = 0.42f;
        bacteria.bit_speed_min = 10.0f;
        bacteria.bit_speed_max = 24.0f;
        bacteria.bit_life_min = 0.16f;
        bacteria.bit_life_max = 0.30f;
        bacteria.bit_drag = 6.50f;
        bacteria.bit_buoyancy = -5.00f;   // heavier debris, drops faster
        bacteria.bit_spin = 6.00f;        // tumbles lazily where the virus whips
        bacteria.bit_scatter = 0.90f;     // a mess, not a shell failing evenly

        bacteria.bloom_count = 2;
        bacteria.bloom_size = 1.80f;
        bacteria.bloom_life = 0.18f;
        bacteria.bloom_alpha = 0.38f;
        bacteria.bloom_speed = 2.60f;
        bacteria.bloom_rise = 0.80f;

        bacteria.inherit_velocity = 0.25f;
    }
};

DeathTables& tables() {
    static DeathTables t;
    return t;
}

u32 family_slot(PathogenFamily family) {
    const u32 i = static_cast<u32>(family);
    return i < kFamilyCount ? i : 0u;
}

} // namespace

const FamilyDeathVfx& family_death_vfx(PathogenFamily family) {
    return tables().look[family_slot(family)];
}

void set_family_death_vfx(PathogenFamily family, const FamilyDeathVfx& look) {
    tables().look[family_slot(family)] = look;
}

} // namespace immune::vfx
