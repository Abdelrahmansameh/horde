// sim/burrow/Burrow.h — burrowing and slithering chaff (the Parasite family).
//
// WHAT IT IS
// A parasite is ordinary chaff -- same SoA slot, same movement kernel, same
// damage paths -- with two additions driven from here:
//
//   BURROW  On a cooldown (+- jitter) it stops, sinks into the tissue where it
//           stands, and some time later comes back up somewhere FURTHER DOWN
//           ITS LANE. While it is under it is untargetable, undamageable and
//           invisible to the crowd. The exit is announced: the same disturbed-
//           tissue mound the dive left behind rises at the exit point for the
//           last `telegraph_duration` seconds before the body breaks out.
//
//   SLITHER Purely cosmetic. The drawn body is a long worm whose travelling
//           wave is advanced by distance walked, so its crests stay put on the
//           ground as the body slides through them -- the way a snake moves,
//           rather than a wiggling sprite. The heading it is drawn at is turned
//           toward the velocity at a bounded rate, so a long body swings round
//           instead of snapping with every shove from the crowd.
//
// Both are per-family data (BurrowParams / SlitherParams, enemies.json's
// "burrow" and "slither" blocks) and every other family ships them disabled,
// so this is a family behaviour switch the way `replicates` is, not a
// subclass (DESIGN.md §8.2).
//
// WHERE THE EXIT GOES
// Candidates are drawn in a CONE, not a circle: `min_range`..`max_range` from
// the agent, within `cone_half_angle` of the lane's forward direction (the flow
// field at the dive point). Each must be walkable, clear of walls, reachable,
// strictly further along the lane (cost-to-goal down by at least
// `min_progress`, so a cone that sweeps across a bend cannot take it
// backwards), and not too close to the objective (`goal_standoff`). Among the
// valid ones the best score wins:
//
//     progress_weight * progress
//   - tower_weight    * (towers whose coverage contains the exit)
//   - crowd_weight    * (pathogens within crowd_radius of the exit)
//
// With the shipped weights, tower coverage dominates (it tries hard to surface
// beyond the defence, as far as its range allows) and crowding is a tiebreak
// (it prefers open ground when it has the choice). No valid candidate: it
// stays up and retries after `retry_delay`.
//
// DETERMINISM
// update() is serial, walks agents in index order and draws only from the sim
// Rng it is handed, so it is a pure function of the sim state at any thread
// count. burrow_state/timer/target are hashed by SimWorld::state_hash(); the
// cosmetic streams (burrow_anim, body_heading, slither_phase) are not.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class ChaffBuffers;
class FlowField;
class DistanceField;
class TissueMask;
class SpatialHash;

/// One family's burrowing, gameplay and look. Every number is authored in
/// enemies.json (game/config/EnemyConfig.cpp); these defaults are what a
/// family that authors nothing gets, and they are OFF.
struct BurrowParams {
    bool enabled = false;

    // ---- Rhythm (seconds) ---------------------------------------------------
    f32 cooldown = 5.0f;              ///< Surface time between burrows.
    f32 cooldown_jitter = 1.5f;       ///< +- uniform on every cooldown, first included.
    f32 dive_duration = 0.7f;         ///< Sinking into the tissue.
    f32 underground_duration = 1.4f;  ///< Gone, telegraph included.
    f32 telegraph_duration = 0.6f;    ///< Tail of underground where the exit mound rises.
    f32 emerge_duration = 0.6f;       ///< Breaking back out.
    f32 retry_delay = 0.5f;           ///< No valid exit: surface time before trying again.

    // ---- Exit search ----------------------------------------------------------
    f32 min_range = 14.0f;            ///< World units from the dive point.
    f32 max_range = 42.0f;            ///< World units; the cone's length.
    f32 cone_half_angle = 55.0f;      ///< Degrees either side of the lane's forward.
    u32 candidate_count = 16;         ///< Exit points sampled per attempt.
    f32 min_progress = 8.0f;          ///< Cost-to-goal the exit must save, world units.
    f32 goal_standoff = 30.0f;        ///< Least cost-to-goal an exit may have.
    f32 wall_clearance = 2.0f;        ///< Least tissue clearance at the exit, world units.
    f32 progress_weight = 1.0f;       ///< Score per world unit of lane gained.
    f32 tower_weight = 100.0f;        ///< Score lost per tower covering the exit.
    f32 tower_range_scale = 1.0f;     ///< Coverage radius = tower range * this.
    f32 crowd_weight = 2.0f;          ///< Score lost per pathogen near the exit.
    f32 crowd_radius = 6.0f;          ///< World units; what "near the exit" means.
    /// Leave its squad on the dive. A squad brakes members that get AHEAD of
    /// its centroid (sim/chaff/ChaffSystem.cpp), which would stall a parasite
    /// on the far side of the defence waiting for a squad that may never come.
    bool leave_squad = true;

    // ---- Look (render only; local sprite units unless stated) ----------------
    f32 mound_radius = 0.17f;         ///< Outer radius of the disturbed-tissue ring.
    f32 hole_radius = 0.08f;          ///< The dark mouth inside it.
    f32 clod_count = 7.0f;            ///< Dirt clods tumbling round the rim.
    f32 clod_size = 0.035f;
    f32 clod_throw = 0.08f;           ///< How far clods are flung past the rim.
    f32 sink_fraction = 0.75f;        ///< Of the dive/emerge, the part the body is moving; the rest is the mound fading.
    Vec4 dirt_color{0.40f, 0.20f, 0.14f, 0.95f};
    Vec4 hole_color{0.08f, 0.04f, 0.03f, 1.0f};
};

/// One family's drawn worm body and the cosmetic sim state that animates it.
struct SlitherParams {
    bool enabled = false;
    f32 wavelength = 7.0f;       ///< World units between body-wave crests on the ground.
    f32 idle_rate = 0.5f;        ///< Wave cycles/sec even when not moving.
    f32 turn_rate = 5.0f;        ///< Radians/sec the drawn heading follows the velocity.
    f32 amplitude = 0.07f;       ///< Local units of sideways swing.
    f32 head_amplitude = 0.4f;   ///< Fraction of `amplitude` left at the head.
    f32 body_length = 0.84f;     ///< Local units, head to tail.
    f32 thickness = 0.085f;      ///< Local half-width at the thickest point.
    f32 segments = 16.0f;        ///< Annulation rings along the body.
    f32 min_speed = 0.25f;       ///< Below this (world units/sec) the heading holds.
};

/// Per-family tables the RENDERER reads (sim never does -- it reads its own
/// copy on BurrowSystem, below, so a config edit cannot change a running sim
/// behind state_hash()'s back except through set_tuning()). Same arrangement
/// as sim/chaff/HitFlash.h: filled by game::apply_enemy_config().
const BurrowParams& family_burrow(PathogenFamily family);
void set_family_burrow(PathogenFamily family, const BurrowParams& params);
const SlitherParams& family_slither(PathogenFamily family);
void set_family_slither(PathogenFamily family, const SlitherParams& params);

struct BurrowTuning {
    BurrowParams burrow[kFamilyCount];
    SlitherParams slither[kFamilyCount];
};

/// A disc of immune coverage an exit would rather not land in.
struct BurrowThreat {
    Vec2 position{0.0f, 0.0f};
    f32 radius = 0.0f;
};

struct BurrowStats {
    u32 dives = 0;        ///< Burrows started this tick.
    u32 emerged = 0;      ///< Agents that finished coming back up this tick.
    u32 failed = 0;       ///< Attempts that found no valid exit.
    u32 burrowed = 0;     ///< Agents in any non-surface state after the update.
};

class BurrowSystem {
public:
    void set_tuning(const BurrowTuning& tuning);
    const BurrowTuning& tuning() const { return tuning_; }
    /// True if any family burrows or slithers; update() is a no-op otherwise.
    bool active() const { return active_; }

    /// Advances every burrowing and slithering agent one tick. Runs after the
    /// chaff movement kernel so a dive freezes the agent where this tick's
    /// movement left it and an exit is scored against this tick's crowd.
    /// `hash` is the tick's spatial hash (positions up to one tick stale,
    /// which is all a crowd estimate needs). `threats` is the towers' coverage.
    BurrowStats update(ChaffBuffers& chaff, const FlowField& flow, const DistanceField& sdf,
                       const TissueMask& mask, const SpatialHash& hash,
                       const std::vector<BurrowThreat>& threats, Rng& rng, f32 dt);

    /// Scores one candidate exit for an agent standing at `from`, whose
    /// cost-to-goal is `from_cost`. Returns false if the point is not a legal
    /// exit at all. Exposed for tests.
    bool score_exit(const BurrowParams& bp, Vec2 from, f32 from_cost, Vec2 exit,
                    const ChaffBuffers& chaff, const FlowField& flow, const DistanceField& sdf,
                    const TissueMask& mask, const SpatialHash& hash,
                    const std::vector<BurrowThreat>& threats, f32& out_score) const;

private:
    BurrowTuning tuning_{};
    bool active_ = false;
};

} // namespace immune::sim
