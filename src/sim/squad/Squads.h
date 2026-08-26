// sim/squad/Squads.h — squad grouping for the chaff horde.
//
// RATIONALE (DESIGN.md §4.2 readability; this file is the answer to it)
// Every chaff agent samples the same level-wide flow field, so every agent
// converges on the same shortest path and the horde arrives as one
// undifferentiated blob. That blob has the fluid feel §4.2 asks for, but at 10k
// agents the player cannot read structure, threat, or timing out of it.
//
// A SQUAD is a group of ~60 agents that follows its own authored path across
// the lane. The partition is enforced by exactly three forces, and only the
// third of them touches the hot neighbour loop:
//
//   1. ANCHOR    Each squad owns a point sliding along a SquadPath. It is
//                LEASHED to the squad's own centroid -- monotonic (never
//                reverses), rate-limited, and always ~lookahead ahead of the
//                mass. A squad jammed behind a tower keeps its anchor close and
//                keeps pushing; the anchor cannot sail off down the path and
//                drag stragglers through a wall trying to follow it.
//
//   2. COHESION  A velocity impulse, decomposed against the flow direction:
//                ACROSS the flow it steers toward the anchor's cross-lane
//                position (which lane of the lane the squad rides), and ALONG
//                the flow it pulls toward the squad's own centre of mass (so
//                the squad does not string out into a ribbon). Both ramp from
//                zero at the squad radius, so an agent inside its own squad
//                feels nothing at all and runs the identical kernel it ran
//                before squads existed -- that is what keeps the fluid feel
//                from being traded away for the grouping.
//
//                Splitting the two axes is what lets the flow field keep its
//                forward authority: the cross-flow term can never push an agent
//                into a wall the field is routing it around, and the along-flow
//                term only speeds up stragglers and reins in leaders.
//
//   3. REPULSION Handled in ChaffSystem's neighbour gather, not here: a
//                neighbour from a different squad gets a larger separation
//                radius and a stronger push. That is what opens a visible gap
//                between two squads rather than letting them merge on contact.
//
// DETERMINISM
// update() accumulates centroids in a SERIAL pass in index order. Float
// addition is not associative, so a parallel reduction would make the centroid
// -- and therefore every agent's steering -- depend on thread count. A linear
// pass over three streams at 16k agents costs microseconds, so there is nothing
// to buy back by parallelizing it.
//
// LAYERING
// immune_sim links only immune_core and EnTT: no JSON, no config. Paths arrive
// as plain structs pushed in by game/level/Level.cpp at load time, exactly the
// arrangement SimWorld::set_spawn_points() already uses.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::sim {

class ChaffBuffers;

/// Sentinel for "this agent belongs to no squad". Agents carrying it steer
/// exactly as they did before squads existed, which is what makes registry
/// exhaustion a graceful degradation rather than a spawn failure.
inline constexpr u16 kNoSquad = 0xFFFFu;

/// One authored (or auto-derived) route across a lane, as a resampled polyline
/// with a precomputed cumulative arclength table so a squad can be positioned
/// by distance travelled rather than by segment index.
struct SquadPath {
    std::string id;
    std::string lane_id;
    /// Resampled polyline. At least 2 points for a usable path.
    std::vector<Vec2> points;
    /// Cumulative arclength; same size as `points`, arc[0] == 0, non-decreasing.
    std::vector<f32> arc;
    /// Lateral tolerance: caps how wide a squad on this path may spread, and
    /// scales the per-squad lateral offset that decorrelates squads sharing it.
    f32 half_width = 6.0f;

    bool valid() const { return points.size() >= 2 && arc.size() == points.size(); }
    f32 length() const { return arc.empty() ? 0.0f : arc.back(); }

    /// World position at arc distance `s`, clamped to the path's ends.
    Vec2 point_at(f32 s) const;
    /// Unit tangent at arc distance `s`; (0,0) only for a degenerate path.
    Vec2 tangent_at(f32 s) const;

    /// Arc position of the point on this path nearest `p`, searched only within
    /// +/- `window` of `hint`.
    ///
    /// Windowed rather than global on purpose. A global nearest-point search is
    /// wrong on a switchback: the two limbs of a hairpin pass close to each
    /// other, so a squad on the outbound limb can be nearest to a point on the
    /// return limb and the anchor would jump across the bend. Searching near
    /// where the squad already is keeps the answer on the limb the squad is
    /// actually travelling, and is cheaper besides.
    f32 project_near(Vec2 p, f32 hint, f32 window) const;

    /// Fills `arc` from `points`. Call after building or mutating `points`.
    void rebuild_arc();
};

/// Tuning for the squad layer. Mirrored into config as `sim.squads`
/// (game/config/GameConfig.h); this struct is the sim-side plain-data form so
/// immune_sim keeps its no-config dependency.
struct SquadTuning {
    /// Master switch. When false, update() still retires squads but every agent
    /// steers exactly as it did before squads existed.
    bool enabled = true;
    /// Agents per squad before the spawner opens a new one.
    u32 target_squad_size = 60;
    /// Hard ceiling on membership, enforced against REPLICATION.
    ///
    /// target_squad_size only governs intake at the spawn point; a daughter is
    /// born on top of its parent mid-lane and used to join its parent's squad
    /// unconditionally. For a replicating family that is a compounding process
    /// with no fixed point -- every member is a source of new members of the
    /// same squad -- so one cohort of 60 grows without bound until the lane is
    /// a single squad again, which is exactly the blob squads exist to break
    /// up. Worse, the steering degrades as it grows: `radius` clamps at
    /// path.half_width, so past ~64 members the cohesion term is pulling an
    /// ever-larger crowd toward a target smaller than the crowd itself.
    ///
    /// Past this count a daughter is stamped kNoSquad and steers on the flow
    /// field alone. That is a deliberate release, not a failure: the parent
    /// squad keeps the shape it was authored to have, and the overflow reads
    /// as a full formation shedding loose stragglers.
    ///
    /// 1.5x target_squad_size leaves replication room to visibly reinforce its
    /// own cohort before it starts shedding. Values below target_squad_size are
    /// raised to it at set_tuning() time -- under it the spawner's own intake
    /// rule would hand out squads that are already over the cap.
    u32 max_squad_size = 90;
    /// Registry capacity. Overflow yields kNoSquad rather than failing a spawn.
    u32 max_squads = 1024;

    /// Ceiling on the flow-vs-anchor blend. Deliberately well below 1 so the
    /// flow field always retains a vote and an anchor across a wall can never
    /// fully capture an agent's steering.
    f32 follow_weight_max = 0.55f;
    /// World units outside the squad radius over which the steer ramps from 0
    /// to follow_weight_max.
    f32 follow_ramp = 6.0f;
    /// Lateral velocity impulse per tick at full weight.
    ///
    /// In the SAME units as ChaffFamilyParams::separation_strength -- a direct
    /// velocity impulse, NOT an acceleration multiplied by dt. That is not a
    /// stylistic choice, it is what the kernel requires to work at all: the
    /// flow term contributes `dir * acceleration * dt` (about 0.33 per tick at
    /// stock tuning) while separation contributes up to `separation_strength *
    /// pressure_max` (about 16). A cohesion term expressed as an acceleration
    /// is two orders of magnitude quieter than the crowd it is trying to steer,
    /// and measurably lost the argument: two squads on paths 16 units apart
    /// settled about 3 units apart, which is inside their own radius and reads
    /// as one blob.
    f32 lateral_push = 3.0f;
    /// Squad radius = squad_radius_scale * sqrt(member_count), clamped to
    /// [min_radius, path.half_width]. Derived from LIVE membership so a squad
    /// being killed off tightens instead of leaving a ghost formation.
    ///
    /// 0.75 is not a taste value: it is exactly the radius ChaffSystem's
    /// phyllotaxis spawn packs `n` agents into (contact_d * sqrt(n) * 0.75,
    /// with contact_d = 1 at stock tuning), i.e. the radius a squad of n
    /// PHYSICALLY occupies. Below that the target is smaller than the bodies
    /// it describes, every member is permanently "outside the squad", the
    /// cohesion steer never switches off, and the promised free interior does
    /// not exist. The first pass used 0.45 and the horde read as one mass for
    /// precisely that reason.
    f32 squad_radius_scale = 0.75f;
    f32 min_radius = 1.5f;

    /// How far ahead of the centroid the anchor sits. This is what makes the
    /// anchor pull rather than merely mark where the squad already is.
    f32 anchor_lookahead = 8.0f;
    /// Leash rate limit, world units/second. Caps how fast arc_pos may advance
    /// so a squad culled down to one leading straggler cannot teleport its
    /// anchor to the front.
    f32 anchor_max_speed = 14.0f;
    /// Half-width of the windowed projection search, in world units.
    f32 project_window = 24.0f;

    /// Separation radius/strength multipliers applied to a neighbour from a
    /// DIFFERENT squad. The radius multiplier is bounded at set_tuning() time so
    /// separation_radius * foreign_radius_mult can never exceed the spatial
    /// hash cell size -- past that the 3x3 cell scan silently misses neighbours.
    f32 foreign_radius_mult = 1.6f;
    f32 foreign_strength_mult = 3.0f;

    /// Per-squad lateral offset as a fraction of path half_width, so two
    /// squads sharing a path do not run exactly nose-to-tail.
    f32 path_lateral_jitter = 0.35f;

    /// Minimum arc distance between two squads opening on the SAME path.
    ///
    /// Round-robin spreads squads across a lane's paths, but a lane has only a
    /// few paths and a wave has dozens of squads, so the fourth squad shares a
    /// path with the first. Without this they are created at the same arc
    /// position -- the spawn point's projection onto that path -- and spawn
    /// inside one another: measured, two same-path squads sat 0.2 world units
    /// apart, which is one blob wearing two ids.
    ///
    /// A new squad is pushed FORWARD along the path past any live squad within
    /// this distance, so same-path squads form a column down the lane. Forward
    /// rather than back because back is off the map: the projection already
    /// sits at the spawn point mouth, and there is no lane behind it.
    ///
    /// Must exceed a squad's DIAMETER or it does not separate anything: at
    /// stock tuning a squad of 60 is ~11.6 across by construction and settles
    /// nearer 16 once the crowd relaxes, so a 12-unit spacing still had
    /// consecutive squads overlapping. 24 is about two diameters, which leaves
    /// a gap of roughly a squad's width between them -- the point at which two
    /// blobs stop reading as one dented blob.
    f32 spawn_spacing = 24.0f;

    /// Consecutive empty ticks before a squad slot is recycled. Non-zero so a
    /// squad mid-spawn (created before its first member lands) is not retired
    /// out from under the spawner.
    u32 retire_ticks = 8;

    /// How far a squad may travel from where it was born and still accept new
    /// members. Past this it is closed and the next arrival opens a fresh squad.
    ///
    /// A squad is a COHORT, not a bucket that stays open until it reaches
    /// target_squad_size. Without this, a spawner filling a 60-strong squad off
    /// a wave ramp of ~1 agent/tick holds it open for a second or more, during
    /// which its first members travel well down the lane while new ones keep
    /// appearing at the spawn point. The centroid is dragged backwards, and
    /// since the along-flow cohesion term steers toward the centroid, the
    /// leaders then physically REVERSE down the lane to rejoin the stragglers.
    /// That is not formation-keeping, it is two unrelated groups being told
    /// they are one.
    ///
    /// Roughly a squad's own diameter: once it has moved its own length, the
    /// spawn point is no longer somewhere it can plausibly still be standing.
    f32 intake_distance = 16.0f;
};

/// One live group. Plain data: the registry owns every one of these and nothing
/// outside sim/squad mutates them.
struct Squad {
    u16 path_index = 0;
    /// Distance travelled along the path. Monotonic non-decreasing for the
    /// lifetime of the squad -- see the anchor rationale in the file header.
    f32 arc_pos = 0.0f;
    /// Fixed lateral offset from the path centerline, in world units.
    f32 lateral = 0.0f;
    /// `arc_pos` at creation. Backs the intake window: once a squad has
    /// travelled `intake_distance` from where it was born it stops taking new
    /// members, so it stays a COHORT rather than an ever-open bucket.
    f32 birth_arc = 0.0f;
    Vec2 anchor{0.0f, 0.0f};
    Vec2 centroid{0.0f, 0.0f};
    u32 member_count = 0;
    f32 radius = 1.5f;
    /// RMS distance from `centroid` over live members -- how wide the squad
    /// ACTUALLY is, as opposed to `radius`, which is the width it is being
    /// steered towards. The gap between the two is the tuning signal: if spread
    /// sits well above radius the cohesion is losing to the crowd, and the
    /// squads will read as one mass however tidy the anchor bookkeeping looks.
    f32 spread = 0.0f;
    u32 empty_ticks = 0;
    bool active = false;
};

/// Owns the level's paths and every live squad. One instance per sim world.
class SquadRegistry {
public:
    void set_tuning(const SquadTuning& tuning);
    const SquadTuning& tuning() const { return tuning_; }

    /// Installs the level's paths and drops every live squad. Called once by
    /// LevelLoader::instantiate(), never from a tick.
    void set_paths(std::vector<SquadPath> paths);
    const std::vector<SquadPath>& paths() const { return paths_; }

    /// Index range of the paths serving `lane_id`, as [first, first+count).
    /// Paths are grouped by lane at set_paths() time so a lane's paths are
    /// contiguous and the tick path never touches a string.
    void lane_path_range(const std::string& lane_id, u32& out_first, u32& out_count) const;

    /// Opens a squad on `path_index`, seeding arc_pos by projecting `spawn_pos`
    /// onto the path. Returns kNoSquad when the registry is full.
    u16 create_squad(u16 path_index, Vec2 spawn_pos);

    /// Next path index for `lane_id`, advancing that lane's round-robin cursor.
    /// Returns false when the lane has no paths.
    bool next_path_for_lane(const std::string& lane_id, u16& out_path_index);

    /// Round-robin restricted to paths whose id is in `allowed`.
    ///
    /// Backs schema 2's per-entry `squad_paths`: it lets a wave entry commit to
    /// the outer paths only -- a flank -- which plain round-robin cannot express
    /// at any count. An EMPTY `allowed`, or one naming nothing on this lane,
    /// falls back to next_path_for_lane(), so the field defaulting to empty is
    /// exactly today's behaviour.
    bool next_path_for_lane_filtered(const std::string& lane_id,
                                     const std::vector<std::string>& allowed,
                                     u16& out_path_index);

    /// One fixed step: recompute centroids/radii from live membership, advance
    /// each anchor along its path, retire empty squads. Runs before the chaff
    /// movement pass so anchors and the positions that produced them agree.
    void update(const ChaffBuffers& chaff, f32 dt);

    bool alive(u16 id) const { return id < squads_.size() && squads_[id].active; }

    /// True while `id` may still take new members: alive, under
    /// target_squad_size, and still within intake_distance of where it was
    /// born. Spawners ask this instead of tracking a fill count of their own,
    /// so every spawn path closes a cohort on the same rule.
    bool accepting(u16 id) const;

    /// True while `id` may absorb a REPLICATED daughter: alive and below
    /// max_squad_size. Deliberately not accepting(): a daughter is born on top
    /// of its parent wherever the squad already is, so the intake DISTANCE
    /// window that closes a spawner cohort is meaningless for it -- only the
    /// size cap is. `pending` is the count of daughters already assigned to
    /// this squad earlier in the same tick, which `member_count` (recomputed
    /// once at the top of the tick) cannot yet see; without it a tick's worth
    /// of replications all read the same stale count and overshoot the cap
    /// together.
    bool can_absorb(u16 id, u32 pending = 0) const;
    const Squad& get(u16 id) const { return squads_[id]; }
    const std::vector<Squad>& squads() const { return squads_; }
    u32 active_count() const { return active_count_; }

    /// Folded into SimWorld::state_hash(): arc_pos and lateral feed steering, so
    /// a determinism assertion that ignored them would not be checking the
    /// squad layer at all.
    u64 state_hash() const;

    void clear();

private:
    SquadTuning tuning_{};
    std::vector<SquadPath> paths_;
    std::vector<Squad> squads_;

    /// Lane grouping over paths_. lane_first_/lane_count_ index into paths_;
    /// lane_cursor_ is that lane's round-robin position.
    std::vector<std::string> lane_ids_;
    std::vector<u32> lane_first_;
    std::vector<u32> lane_count_;
    std::vector<u32> lane_cursor_;

    /// Serial number handed to each squad ever created, so a squad's lateral
    /// offset is a pure function of creation order rather than of slot reuse.
    u32 next_serial_ = 0;
    u32 active_count_ = 0;

    /// Centroid accumulators, sized to squads_ and reused every tick so update()
    /// allocates nothing once warm.
    std::vector<f32> sum_x_;
    std::vector<f32> sum_y_;
    std::vector<f32> sum_d2_;
    std::vector<u32> sum_n_;

    i32 lane_index(const std::string& lane_id) const;
};

} // namespace immune::sim
