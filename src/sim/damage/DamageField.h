// sim/damage/DamageField.h — aggregate damage. FROZEN CONTRACT.
// Owner: Wave 2A.
//
// RATIONALE (DESIGN.md §8.2 + §7)
// Chaff is never hit individually. There are no projectiles-per-agent, no hit
// registration, no per-agent HP pool. Instead a tower publishes a *damage
// field*: a region plus a kill rate. Each tick the field asks the spatial hash
// which cells it overlaps and thins the chaff inside by that rate.
//
// This is simultaneously:
//   - the performance answer: cost scales with the number of *fields* and the
//     cells they cover, not with 10,000 x towers pair tests; and
//   - the art direction: mass removed at a field's boundary is exactly the
//     "edge erosion" language §7 asks for. The visual is the mechanic.
//
// TWO THINNING MODES (DESIGN.md §10 open question)
// Both are implemented behind one switch so the feel pass can pick by playing:
//   - DensityThinning  — deterministic. Subtracts kill_rate * dt from every
//     overlapped agent's density. Smooth, perfectly predictable, reads as the
//     mass "dissolving". Preferred default.
//   - ProbabilisticRemoval — each overlapped agent rolls to be removed whole.
//     Grainier, more "popping", cheaper per agent. Requires an Rng and so is
//     order-sensitive; it draws from a per-field forked stream to stay
//     deterministic.
//
// ACCOUNTING
// Removed density is attributed back to the owning tower and to the economy
// (ATP per kill). `DamageStats` is what the economy and the HUD read; nothing
// else may infer kills by diffing agent counts.
#pragma once

#include "core/Types.h"
#include "sim/Attribution.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class ChaffBuffers;
class SpatialHash;

enum class ThinningMode : u8 {
    DensityThinning = 0,      ///< Deterministic; default.
    ProbabilisticRemoval = 1, ///< Whole-agent rolls.
};

enum class FieldShape : u8 {
    Circle = 0,
    Rect = 1,
    /// Cone from `origin` along `direction` with half-angle `arc_radians`.
    Cone = 2,
    /// Chain-jump volume: the Complement Cascade resolves to a sequence of
    /// small circles built at evaluation time; stored as Circle links.
    Chain = 3,
};

/// A published damage region. Towers create/refresh these; the damage system
/// owns evaluation. Trivially copyable and stored in a flat vector.
struct DamageField {
    FieldShape shape = FieldShape::Circle;
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 0.0f;              ///< Circle/Chain radius.
    Rect rect{};                    ///< Rect shape only.
    Vec2 direction{1.0f, 0.0f};     ///< Cone shape only (unit).
    f32 arc_radians = 0.0f;         ///< Cone half-angle.

    /// Density removed per second from each agent inside the field. This is the
    /// single tuning knob for tower power against chaff.
    f32 kill_rate = 0.0f;

    /// Falloff exponent applied to normalized distance from the field centre.
    /// 0 = flat (full rate everywhere), 1 = linear, 2 = quadratic.
    f32 falloff = 0.0f;

    /// Bitmask of PathogenFamily bits this field affects. 0xFF = all families.
    /// Lets NK Cells hit only kHidden targets, Cytotoxic T favour elites, etc.
    u8 family_mask = 0xFF;

    /// Multiplier applied to agents carrying chaff_flags::kMarked (the Goblet
    /// Cell's weaken debuff). Most casters set this to
    /// chaff_flags::kMarkedDamageMultiplier; left at 1.0 it is a no-op, which is
    /// the right default for a field that should not care whether its target is
    /// weakened (friendly-fire hazards, for instance).
    f32 marked_multiplier = 1.0f;

    /// Seconds remaining. <= 0 means "persistent, refreshed by its owner each
    /// tick"; > 0 counts down and the field self-removes (NETs, spore hazards).
    f32 lifetime = 0.0f;

    /// Owning tower entity, for kill attribution. Invalid for environmental
    /// hazards (fungal death clouds, allergen self-damage).
    EntityId owner{};

    /// Set for fields that damage the *player's* own units/objective — the
    /// allergen overreaction mechanic (DESIGN.md §6).
    bool friendly_fire = false;
};

struct DamageStats {
    f32 density_removed = 0.0f;   ///< Total density destroyed this tick.
    u32 agents_killed = 0;        ///< Agents whose density reached zero.
    u32 fields_evaluated = 0;
    u32 cells_touched = 0;
    /// Per-family density removed; the HUD colour-codes the kill feed by this.
    f32 density_removed_by_family[kFamilyCount] = {};
};

class DamageSystem {
public:
    void set_mode(ThinningMode mode) { mode_ = mode; }
    ThinningMode mode() const { return mode_; }

    /// Attaches (or detaches, with null) the per-owner accounting sink. Null by
    /// default: only the balance harness sets one. See sim/Attribution.h for
    /// why this costs nothing when off and changes nothing when on.
    void set_attribution(DamageAttribution* sink) { attribution_ = sink; }
    DamageAttribution* attribution() const { return attribution_; }

    /// Registers a field for this tick. Persistent fields (lifetime <= 0) must
    /// be re-submitted each tick by their owner; timed fields are retained.
    /// Returns an index valid only until the next `clear_transient()`.
    u32 submit(const DamageField& field);

    /// Drops fields whose lifetime expired and all lifetime<=0 fields, ready
    /// for the next tick's submissions. Called once per tick after `apply`.
    void clear_transient(f32 dt);

    /// Evaluates every registered field against the chaff store via the spatial
    /// hash and thins accordingly. Does NOT compact — the caller runs
    /// ChaffBuffers::compact() once, after all damage sources have applied.
    DamageStats apply(ChaffBuffers& chaff, const SpatialHash& hash, Rng& rng, f32 dt);

    /// Density of chaff currently inside a region, without damaging it.
    /// Mast Cell trigger and the HUD threat overlay use this.
    f32 measure_density(const ChaffBuffers& chaff, const SpatialHash& hash,
                        const DamageField& region) const;

    const std::vector<DamageField>& fields() const { return fields_; }

    /// The fields exactly as `apply()` last saw them — the correct list for
    /// anything running BETWEEN ticks, which in practice means the renderer.
    ///
    /// `fields()` is not that list. It is the submission buffer, and
    /// clear_transient() runs at the END of SimWorld::tick(), so by the time a
    /// frame is drawn every persistent field has already been dropped on the
    /// grounds that its owner will re-submit next tick. That is right for the
    /// sim and wrong for the screen: the Cryo cone, the Laser beam and the NK
    /// rotor are all persistent, so a renderer reading fields() sees the three
    /// AoEs that are permanently on as permanently absent, and those towers
    /// draw no attack at all. This snapshot is taken before that cull.
    const std::vector<DamageField>& rendered_fields() const { return rendered_; }

    void clear_all() {
        fields_.clear();
        rendered_.clear();
    }

    /// Reserves field storage so submit() never allocates during a tick.
    void reserve(usize max_fields) {
        fields_.reserve(max_fields);
        rendered_.reserve(max_fields);
    }

private:
    std::vector<DamageField> fields_;
    std::vector<DamageField> rendered_;
    ThinningMode mode_ = ThinningMode::DensityThinning;
    DamageAttribution* attribution_ = nullptr;
};

} // namespace immune::sim
