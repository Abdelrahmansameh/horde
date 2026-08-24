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

/// Resolves a SpawnEntry's spawn_point_id against the level's runtime spawn
/// point list. Empty id (or no match) falls back to the first spawn point,
/// per WaveDirector.h's doc comment on SpawnEntry::spawn_point_id. Returns
/// false if the level has no spawn points at all (headless CLI modes that
/// never loaded a real level).
bool resolve_spawn_point(const sim::SimWorld& world, const std::string& spawn_point_id,
                    Vec2& out_pos, f32& out_radius, std::string& out_lane) {
    const auto& spawn_points = world.spawn_points();
    if (spawn_points.empty()) return false;
    if (!spawn_point_id.empty()) {
        for (const auto& p : spawn_points) {
            if (p.id == spawn_point_id) {
                out_pos = p.position;
                out_radius = p.radius;
                out_lane = p.lane_id;
                return true;
            }
        }
    }
    out_pos = spawn_points[0].position;
    out_radius = spawn_points[0].radius;
    out_lane = spawn_points[0].lane_id;
    return true;
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
    clearing_elapsed_ = 0.0f;
    early_start_requested_ = false;
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

                Vec2 spawn_point_pos{};
                f32 spawn_point_radius = 1.0f;
                std::string spawn_point_lane;
                if (!resolve_spawn_point(world, e.spawn_point_id, spawn_point_pos, spawn_point_radius, spawn_point_lane)) {
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
                    const u32 squad_size = math::max(reg.tuning().target_squad_size, 1u);
                    const bool grouping = reg.tuning().enabled && !reg.paths().empty();
                    SquadCursor& cursor = squad_cursor_[i];

                    u32 spawned = 0;
                    u32 left = to_spawn;
                    while (left > 0) {
                        u16 squad = sim::kNoSquad;
                        u32 chunk = left;
                        // Where this chunk actually goes. Ungrouped chaff still
                        // comes out of the spawn point disc exactly as before.
                        Vec2 at = spawn_point_pos;
                        f32 at_radius = spawn_point_radius;

                        if (grouping) {
                            // accepting() closes a squad both when it is full
                            // AND when it has travelled away from where it was
                            // born -- see SquadTuning::intake_distance. A ramp
                            // that trickles agents out over seconds must not keep
                            // pouring them into a cohort already halfway down the
                            // lane: the centroid gets dragged back to the spawn
                            // point and the leaders reverse to rejoin it.
                            if (cursor.squad == sim::kNoSquad ||
                                !reg.accepting(cursor.squad)) {
                                u16 path = 0;
                                cursor.squad = reg.next_path_for_lane(spawn_point_lane, path)
                                                   ? reg.create_squad(path, spawn_point_pos)
                                                   : sim::kNoSquad;
                                cursor.filled = 0;
                            }
                            squad = cursor.squad;
                            // A full registry yields kNoSquad; those agents
                            // spawn ungrouped rather than stalling the wave.
                            if (squad != sim::kNoSquad) {
                                chunk = math::min(left, squad_size - cursor.filled);
                                // Cursor can be stale against a squad the registry
                                // already considers full; next pass reopens one.
                                if (chunk == 0) chunk = left;

                                // SPAWN ON THE SQUAD'S OWN PATH, not at the
                                // spawn point centre. create_squad() has already
                                // seeded the anchor by projecting the spawn
                                // point onto this squad's path and applying the
                                // squad's lateral offset, so the anchor is a
                                // point on the lane, near the spawn point, that
                                // no other squad is using.
                                //
                                // This matters more than it looks. Spawning
                                // every squad in one disc means they all start
                                // interpenetrated and have to shove each other
                                // apart before any of them reads as a group --
                                // which, in a lane only a squad or two wide,
                                // they never finish doing. Starting them apart
                                // means they are grouped from the first frame
                                // and the steering only has to KEEP them so.
                                const sim::Squad& sq = reg.get(squad);
                                at = sq.anchor;
                                // Zero, deliberately: spawn_burst grows the
                                // disc to exactly what this count needs at
                                // contact spacing, which IS a squad-sized
                                // clump. Passing the spawn point radius instead
                                // would smear ~60 agents over a disc several
                                // times the squad's own radius, and they would
                                // spend the first seconds contracting rather
                                // than reading as a group.
                                at_radius = 0.0f;
                            }
                        }
                        const u32 got = world.chaff_system().spawn_burst(
                            world.chaff(), e.family, at, at_radius, chunk, rng, squad);
                        spawned += got;
                        if (squad != sim::kNoSquad) cursor.filled += got;
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
                        const Vec2 jitter = rng.unit_disc() * spawn_point_radius;
                        const sim::named::SpawnParams params{e.elite_id, spawn_point_pos + jitter,
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
