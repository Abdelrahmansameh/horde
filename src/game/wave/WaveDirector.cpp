// game/wave/WaveDirector.cpp — implementation of the frozen WaveDirector.h
// contract. Owner: Wave 3A. This is the real, permanent
// Prep->Spawning->Clearing->Complete state machine and nothing else: the
// director schedules and spawns a table it is handed, and no longer builds
// one. Wave tables come from the level file (Level.h, AUTHORED WAVES) --
// the region-keyed generator and its assets/config/waves.json tuning are gone.
#include "game/wave/WaveDirector.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/squad/Squads.h"

namespace immune::game {

namespace {

/// Resolves a SpawnEntry's spawn point against the level's runtime list.
///
/// A named point remains a deliberate fixed override. An empty id means the
/// author wants the entry's successive squads (and unsquadded bursts/elites)
/// to walk every placed marker in order. Keeping the cursor per SpawnEntry
/// makes the pattern deterministic and lets multiple concurrent entries each
/// own their rotation. Unknown named ids retain the old safe fallback to point
/// zero; level validation reports that authoring error before a playable run.
const sim::SpawnPointRuntime* resolve_spawn_point(const sim::SimWorld& world,
                                                  const std::string& spawn_point_id,
                                                  u32& round_robin_cursor) {
    const auto& spawn_points = world.spawn_points();
    if (spawn_points.empty()) return nullptr;
    if (!spawn_point_id.empty()) {
        for (const auto& p : spawn_points) {
            if (p.id == spawn_point_id) {
                return &p;
            }
        }
        return &spawn_points[0];
    }
    const u32 index = round_robin_cursor % static_cast<u32>(spawn_points.size());
    round_robin_cursor = (index + 1u) % static_cast<u32>(spawn_points.size());
    return &spawn_points[index];
}

f32 spawn_entry_end(const SpawnEntry& e) { return e.start_time + math::max(e.duration, 0.0f); }

} // namespace

void WaveDirector::set_waves(std::vector<WaveDef> waves) { waves_ = std::move(waves); }

void WaveDirector::start(sim::SimWorld& world) {
    (void)world;
    status_ = WaveStatus{};
    status_.all_waves_complete = waves_.empty();
    if (!waves_.empty()) {
        status_.phase = WavePhase::Prep;
        status_.phase_time_remaining = waves_[0].prep_time;
    }
    wave_time_ = 0.0f;
    spawned_so_far_.clear();
    squad_cursor_.clear();
    spawn_point_cursor_.clear();
    clearing_elapsed_ = 0.0f;
    early_start_requested_ = false;
    // A wave reward the previous run cleared but never drained (the run ended,
    // or the player restarted between the clear and the next tick) is not this
    // run's ATP.
    pending_atp_reward_ = 0;
}

void WaveDirector::request_early_start() { early_start_requested_ = true; }

const WaveDef* WaveDirector::next_wave() const {
    const usize next = static_cast<usize>(status_.wave_index) + 1u;
    return next < waves_.size() ? &waves_[next] : nullptr;
}

u32 WaveDirector::take_pending_atp_reward() {
    const u32 r = pending_atp_reward_;
    pending_atp_reward_ = 0;
    return r;
}

void WaveDirector::tick(sim::SimWorld& world, Rng& rng, f32 dt) {
    if (waves_.empty() || status_.wave_index >= waves_.size()) {
        status_.all_waves_complete = true;
        status_.phase = WavePhase::Complete;
        return;
    }
    const WaveDef& wave = waves_[status_.wave_index];

    switch (status_.phase) {
        case WavePhase::Prep: {
            status_.phase_time_remaining -= dt;
            if (status_.phase_time_remaining <= 0.0f || early_start_requested_) {
                early_start_requested_ = false;
                status_.phase = WavePhase::Spawning;
                wave_time_ = 0.0f;
                spawned_so_far_.assign(wave.spawns.size(), 0u);
                squad_cursor_.assign(wave.spawns.size(), SquadCursor{});
                spawn_point_cursor_.assign(wave.spawns.size(), 0u);
                status_.remaining_to_spawn = 0;
                for (const SpawnEntry& e : wave.spawns) status_.remaining_to_spawn += e.count;
                status_.phase_time_remaining = 0.0f;
            }
            break;
        }

        case WavePhase::Spawning: {
            wave_time_ += dt;
            bool all_entries_done = true;

            for (usize i = 0; i < wave.spawns.size(); ++i) {
                const SpawnEntry& e = wave.spawns[i];
                if (wave_time_ < spawn_entry_end(e)) all_entries_done = false;

                // Linear ramp: how many of this entry's count are "due" by now.
                const f32 span = math::max(e.duration, 1e-4f);
                const f32 t = math::clamp((wave_time_ - e.start_time) / span, 0.0f, 1.0f);
                const u32 due = static_cast<u32>(t * static_cast<f32>(e.count));
                if (due <= spawned_so_far_[i]) continue;
                const u32 to_spawn = due - spawned_so_far_[i];

                if (world.spawn_points().empty()) {
                    // No spawn points at all (headless mode with no level) -- nothing
                    // to spawn from; count this entry done so the wave can't
                    // stall waiting for spawns that will never happen.
                    const u32 remaining_here = e.count - spawned_so_far_[i];
                    spawned_so_far_[i] = e.count;
                    status_.remaining_to_spawn -=
                        math::min(remaining_here, status_.remaining_to_spawn);
                    continue;
                }

                if (e.elite_id == 0) {
                    // Chop this tick's release into squad-sized bursts. Each
                    // burst is its own spawn_burst() call so the phyllotaxis
                    // packing still applies per squad rather than smearing one
                    // spiral across several of them.
                    sim::SquadRegistry& reg = world.squads();
                    // schema 2: the ENTRY may set its own squad size. 0 keeps
                    // the global, which is what every pre-v2 level means. 900
                    // arriving as 15 squads of 60 and 900 arriving as 6 squads
                    // of 150 are different silhouettes reaching the line at
                    // different times, and this is the only way to ask for the
                    // second.
                    const u32 squad_size =
                        math::max(e.squad_size != 0 ? e.squad_size
                                                    : reg.tuning().target_squad_size,
                                  1u);
                    const bool grouping = reg.tuning().enabled && !reg.paths().empty();
                    SquadCursor& cursor = squad_cursor_[i];

                    u32 spawned = 0;
                    u32 left = to_spawn;
                    while (left > 0) {
                        u16 squad = sim::kNoSquad;
                        u32 chunk = math::min(left, squad_size);
                        // Where this chunk actually goes. Ungrouped chaff still
                        // comes out of the authored spawn-point disc. Splitting
                        // it at squad size means an ungrouped horde also cycles
                        // cleanly through every available marker.
                        const sim::SpawnPointRuntime* spawn_point = nullptr;

                        if (grouping) {
                            // accepting() closes a squad both when it is full
                            // AND when it has travelled away from where it was
                            // born -- see SquadTuning::intake_distance. A ramp
                            // that trickles agents out over seconds must not keep
                            // pouring them into a cohort already halfway down the
                            // lane: the centroid gets dragged back to the spawn
                            // point and the leaders reverse to rejoin it.
                            if (cursor.squad == sim::kNoSquad || cursor.filled >= squad_size ||
                                !reg.accepting(cursor.squad)) {
                                spawn_point = resolve_spawn_point(world, e.spawn_point_id,
                                                                  spawn_point_cursor_[i]);
                                u16 path = 0;
                                // schema 2: an entry may restrict itself to a
                                // named subset of the lane's paths. Empty (the
                                // default, and every pre-v2 level) is the plain
                                // lane rotation.
                                cursor.squad =
                                    spawn_point && reg.next_path_for_lane_filtered(spawn_point->lane_id,
                                                                    e.squad_paths, path)
                                        ? reg.create_squad(path, spawn_point->position)
                                        : sim::kNoSquad;
                                cursor.filled = 0;
                                cursor.pattern_phase = rng.range_f(0.0f, math::kTwoPi);
                                if (spawn_point) {
                                    cursor.spawn_point_index = static_cast<u32>(
                                        spawn_point - world.spawn_points().data());
                                }
                            } else {
                                cursor.spawn_point_index %= static_cast<u32>(world.spawn_points().size());
                                spawn_point = &world.spawn_points()[cursor.spawn_point_index];
                            }
                            squad = cursor.squad;
                            // A full registry yields kNoSquad; those agents
                            // spawn ungrouped rather than stalling the wave.
                            if (squad != sim::kNoSquad) {
                                chunk = math::min(left, squad_size - cursor.filled);
                                // Cursor can be stale against a squad the registry
                                // already considers full; next pass reopens one.
                                if (chunk == 0) chunk = left;

                                // Spawn at the AUTHOR'S marker, exactly. The
                                // path projection inside create_squad() is
                                // still useful to establish the direction the
                                // newly born squad will travel, but it must
                                // never rewrite where it appears. This keeps a
                                // marker placed at a tactical choke, doorway,
                                // or ambush point visibly and mechanically
                                // authoritative.
                                //
                                // The authored radius remains the formation's
                                // requested opening footprint. spawn_burst()
                                // centres that footprint on `at`, growing it
                                // only when contact spacing requires more room.
                                const Vec2 at = spawn_point->position;
                                const u32 got = world.chaff_system().spawn_burst(
                                    world.chaff(), e.family, at, spawn_point->radius, chunk, rng,
                                    squad, cursor.filled, squad_size,
                                    cursor.pattern_phase);
                                spawned += got;
                                cursor.filled += got;
                                left -= chunk;
                                if (got < chunk) break;
                                continue;
                            }
                        }
                        if (!spawn_point) {
                            spawn_point = resolve_spawn_point(world, e.spawn_point_id,
                                                              spawn_point_cursor_[i]);
                        }
                        const u32 got = world.chaff_system().spawn_burst(
                            world.chaff(), e.family, spawn_point->position, spawn_point->radius,
                            chunk, rng, squad);
                        spawned += got;
                        left -= chunk;
                        if (got < chunk) break;   // at capacity; stop trying this tick
                    }

                    spawned_so_far_[i] += spawned;
                    status_.remaining_to_spawn -=
                        math::min(spawned, status_.remaining_to_spawn);
                    // spawn_burst may spawn fewer than asked (capacity). Count
                    // the shortfall as done too, so a full chaff buffer can't
                    // stall the wave forever waiting for room that never frees.
                    if (spawned < to_spawn) spawned_so_far_[i] += (to_spawn - spawned);
                } else {
                    for (u32 n = 0; n < to_spawn; ++n) {
                        const sim::SpawnPointRuntime* spawn_point =
                            resolve_spawn_point(world, e.spawn_point_id, spawn_point_cursor_[i]);
                        const Vec2 jitter = rng.unit_disc() * spawn_point->radius;
                        const sim::named::SpawnParams params{e.elite_id, spawn_point->position + jitter,
                                                              1.0f, 0.0f};
                        const EntityId id = sim::named::spawn(world, params);
                        ++spawned_so_far_[i];
                        if (status_.remaining_to_spawn > 0) --status_.remaining_to_spawn;
                        if (!id.valid()) break; // named-agent cap reached; stop trying this tick
                    }
                }
            }

            if (all_entries_done && status_.remaining_to_spawn == 0) {
                status_.phase = WavePhase::Clearing;
                clearing_elapsed_ = 0.0f;
            }
            break;
        }

        case WavePhase::Clearing: {
            clearing_elapsed_ += dt;
            // "Cleared" means the horde is gone, or a generous grace period
            // has elapsed so a few unreachable/leaked stragglers can never
            // stall the wave sequence forever.
            const bool horde_gone = world.chaff().total_density() <= 0.0f;
            constexpr f32 kClearingTimeout = 60.0f;
            if (horde_gone || clearing_elapsed_ >= kClearingTimeout) {
                pending_atp_reward_ += wave.atp_reward;
                const usize next_index = static_cast<usize>(status_.wave_index) + 1u;
                if (next_index < waves_.size()) {
                    status_.wave_index = static_cast<u32>(next_index);
                    status_.phase = WavePhase::Prep;
                    status_.phase_time_remaining = waves_[next_index].prep_time;
                } else {
                    status_.phase = WavePhase::Complete;
                    status_.all_waves_complete = true;
                }
            }
            break;
        }

        case WavePhase::Complete:
            status_.all_waves_complete = true;
            break;
    }
}

} // namespace immune::game
