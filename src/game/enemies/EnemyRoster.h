// game/enemies/EnemyRoster.h — pathogen family and elite definitions.
// FROZEN CONTRACT. Owner: Wave 2C.
//
// RATIONALE (DESIGN.md §6)
// The readability rule — colour = family, silhouette size = threat tier,
// animation tempo = speed tier — is enforced *structurally* here: those three
// properties are separate fields, sourced from the family/tier/speed tables, so
// no individual archetype can quietly break the visual language.
//
// Chaff behaviour is NOT a subclass. Family-specific behaviour is expressed as
// ChaffFamilyParams values plus chaff flag bits consumed by the one movement
// kernel. Only elites (which are ECS entities) get real per-archetype logic.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; struct ChaffTuning; }

namespace immune::game {

enum class ThreatTier : u8 { Chaff = 0, Elite = 1, Boss = 2 };
enum class SpeedTier : u8 { Slow = 0, Normal = 1, Fast = 2, Erratic = 3 };

/// Static description of one pathogen family (DESIGN.md §6 table).
struct FamilyDef {
    PathogenFamily family = PathogenFamily::Virus;
    const char* name = "";
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   ///< Must equal render::family_color.
    SpeedTier speed_tier = SpeedTier::Normal;
    f32 base_density = 1.0f;
    /// Behaviour switches consumed by the chaff kernel, not by subclassing:
    bool replicates = false;   ///< Virus: exponential pressure.
    bool clumps = false;       ///< Bacteria: biofilm.
    bool drifts = false;       ///< Fungal spore.
    bool can_hide = false;     ///< Parasite.
    bool leaves_hazard = false;///< Fungal spore death cloud.
};

/// One named elite/boss archetype.
struct EliteDef {
    u16 id = 0;
    const char* name = "";
    PathogenFamily family = PathogenFamily::Parasite;
    ThreatTier tier = ThreatTier::Elite;
    f32 max_health = 500.0f;
    f32 armor = 0.0f;
    f32 speed = 3.0f;
    f32 sprite_size = 2.0f;    ///< Silhouette size encodes threat tier.
    f32 ability_cooldown = 6.0f;
    f32 telegraph_duration = 0.8f;
    u32 atp_bounty = 50;
};

class EnemyRoster {
public:
    /// Populates the canonical six families and the elite table.
    void load_defaults();

    const FamilyDef& family(PathogenFamily f) const;
    const std::vector<EliteDef>& elites() const { return elites_; }
    const EliteDef* find_elite(std::string_view name) const;

    /// Fills a ChaffTuning from the family table so the sim kernel and the
    /// roster can never disagree.
    void apply_to_tuning(sim::ChaffTuning& tuning) const;

    /// Spawns a named elite as an ECS entity at `pos`. Invalid id on failure.
    EntityId spawn_elite(sim::SimWorld& world, u16 elite_id, Vec2 pos) const;

    /// Registers the enemy behaviour ECS systems.
    void register_systems(sim::SimWorld& world);

private:
    FamilyDef families_[kFamilyCount]{};
    std::vector<EliteDef> elites_;
};

} // namespace immune::game
