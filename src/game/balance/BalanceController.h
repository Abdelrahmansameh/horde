#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"

#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace immune::sim { class SimWorld; }

namespace immune::game {
class Economy;
class TowerSystem;
class WaveDirector;
struct LevelDef;
}

namespace immune::game::balance {

/// The runner applies and records these intents through the normal game APIs.
/// Recorded scripts/replays use this same representation; a controller never
/// spends ATP, casts, starts waves, or consumes the world's RNG itself.
enum class ActionKind : u8 { Deploy = 0, CastAbility, StartWave };

struct Action {
    Tick tick = 0;
    ActionKind kind = ActionKind::Deploy;
    TowerType tower = TowerType::Neutrophil;
    AbilityId ability = AbilityId::ComplementCascadeBurst;
    Vec2 position{0.0f, 0.0f};
    u32 quantity = 1;
};

/// These are transparent heuristic strategies, not calibrated human skill
/// ratings. None still starts waves, making an undefended control runnable.
enum class Policy : u8 { None = 0, Novice, Competent, Tactical, Specialist };

struct ControllerParams {
    Policy policy = Policy::Competent;
    /// Multipliers on the policy's composition; Specialist uses these directly.
    /// Zero prohibits that type, including when other types are unaffordable.
    std::array<f32, kTowerTypeCount> weights{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    /// Zero selects the policy default. Nonzero values are explicit experiments.
    u32 reaction_ticks = 0;
    u32 prep_ticks = 0;
    u32 reserve_atp = 0;
    /// Disable emergency casts for unit/composition ablations. Unlocks remain
    /// part of the real progression profile; this only changes policy behavior.
    bool use_abilities = true;
    /// Perturbs candidate sampling and ties using a private integer hash. It
    /// never changes gameplay RNG or exposes future random outcomes.
    u64 placement_seed = 1;
};

struct ControllerView {
    const sim::SimWorld& world;
    const LevelDef& level;
    const TowerSystem& towers;
    const Economy& economy;
    const WaveDirector& waves;
    const ActiveAbilitySystem& abilities;
    /// Experimental purchase caps constrain the same budget observation as ATP.
    u64 spending_remaining = std::numeric_limits<u64>::max();
};

class BalanceController {
public:
    explicit BalanceController(ControllerParams params = {});
    /// Call before the session step. At most one purchase and one cast are
    /// proposed per reaction. Only living visible enemies inside the playable
    /// world contribute pressure; burrow state, pending spawns and RNG are not
    /// inspected. Authored geometry and the current wave preview are public.
    std::vector<Action> decide(const ControllerView& view);
    void reset();

private:
    struct Candidate {
        Vec2 position{0.0f, 0.0f};
        f32 priority = 1.0f;
        f32 progress = 0.5f;
    };
    struct SharedIntercept {
        Vec2 entrance{0.0f, 0.0f};
        Vec2 position{0.0f, 0.0f};
        bool viable = false;
        bool has_fork = false;
    };
    struct NavigationApproach {
        Vec2 entrance{0.0f, 0.0f};
        std::vector<std::vector<Vec2>> branches;
    };

    void build_candidates(const ControllerView& view);

    ControllerParams params_;
    std::vector<Candidate> candidates_;
    std::vector<SharedIntercept> shared_intercepts_;
    std::vector<NavigationApproach> navigation_approaches_;
    Tick next_navigation_refresh_ = 0;
    u32 navigation_wave_ = std::numeric_limits<u32>::max();
    const LevelDef* candidate_level_ = nullptr;
    Tick next_decision_ = 0;
    Tick next_ability_ = 0;
    Tick prep_since_ = 0;
    u32 observed_wave_ = 0;
    u8 observed_phase_ = 0;
    bool observed_ = false;
};

bool parse_policy(std::string_view text, Policy& policy);
const char* policy_name(Policy policy);
const char* action_kind_name(ActionKind kind);

} // namespace immune::game::balance
