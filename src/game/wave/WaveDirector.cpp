// game/wave/WaveDirector.cpp — implementation of the frozen WaveDirector.h
// contract. Owner: Wave 3A (full per-region tables/tuning); this is the real,
// permanent Prep->Spawning->Clearing->Complete state machine and a minimal
// but genuine generate(), not a placeholder.
#include "game/wave/WaveDirector.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/ecs/NamedAgents.h"

namespace immune::game {

namespace {

/// Resolves a SpawnEntry's portal_id against the level's runtime portal list.
/// Empty id (or no match) falls back to the first portal, per WaveDirector.h's
/// doc comment on SpawnEntry::portal_id. Returns false if the level has no
/// portals at all (headless CLI modes that never loaded a real level).
bool resolve_portal(const sim::SimWorld& world, const std::string& portal_id,
                    Vec2& out_pos, f32& out_radius) {
    const auto& portals = world.portals();
    if (portals.empty()) return false;
    if (!portal_id.empty()) {
        for (const auto& p : portals) {
            if (p.id == portal_id) {
                out_pos = p.position;
                out_radius = p.radius;
                return true;
            }
        }
    }
    out_pos = portals[0].position;
    out_radius = portals[0].radius;
    return true;
}

f32 spawn_entry_end(const SpawnEntry& e) { return e.start_time + math::max(e.duration, 0.0f); }

} // namespace

void WaveDirector::set_waves(std::vector<WaveDef> waves) { waves_ = std::move(waves); }

std::vector<WaveDef> WaveDirector::generate(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = (i == 0) ? 8.0f : 15.0f;
        w.atp_reward = 50 + i * 10;

        const u32 base = 40 + i * 25;

        SpawnEntry virus;
        virus.family = PathogenFamily::Virus;
        virus.count = base + static_cast<u32>(rng.range_f(0.0f, 10.0f));
        virus.start_time = 0.0f;
        virus.duration = 6.0f;
        w.spawns.push_back(virus);

        if (i >= 1) {
            SpawnEntry bacteria;
            bacteria.family = PathogenFamily::Bacteria;
            bacteria.count = base / 3;
            bacteria.start_time = 3.0f;
            bacteria.duration = 5.0f;
            w.spawns.push_back(bacteria);
        }
        if (i >= 3) {
            SpawnEntry fungal;
            fungal.family = PathogenFamily::FungalSpore;
            fungal.count = base / 4;
            fungal.start_time = 2.0f;
            fungal.duration = 8.0f;
            w.spawns.push_back(fungal);
        }

        waves.push_back(std::move(w));
    }
    return waves;
}

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
    clearing_elapsed_ = 0.0f;
    early_start_requested_ = false;
}

void WaveDirector::request_early_start() { early_start_requested_ = true; }

const WaveDef* WaveDirector::next_wave() const {
    const usize next = static_cast<usize>(status_.wave_index) + 1u;
    return next < waves_.size() ? &waves_[next] : nullptr;
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

                Vec2 portal_pos{};
                f32 portal_radius = 1.0f;
                if (!resolve_portal(world, e.portal_id, portal_pos, portal_radius)) {
                    // No portals at all (headless mode with no level) -- nothing
                    // to spawn from; count this entry done so the wave can't
                    // stall waiting for spawns that will never happen.
                    const u32 remaining_here = e.count - spawned_so_far_[i];
                    spawned_so_far_[i] = e.count;
                    status_.remaining_to_spawn -=
                        math::min(remaining_here, status_.remaining_to_spawn);
                    continue;
                }

                if (e.elite_id == 0) {
                    const u32 spawned = world.chaff_system().spawn_burst(
                        world.chaff(), e.family, portal_pos, portal_radius, to_spawn, rng);
                    spawned_so_far_[i] += spawned;
                    status_.remaining_to_spawn -=
                        math::min(spawned, status_.remaining_to_spawn);
                    // spawn_burst may spawn fewer than asked (capacity). Count
                    // the shortfall as done too, so a full chaff buffer can't
                    // stall the wave forever waiting for room that never frees.
                    if (spawned < to_spawn) spawned_so_far_[i] += (to_spawn - spawned);
                } else {
                    for (u32 n = 0; n < to_spawn; ++n) {
                        const Vec2 jitter = rng.unit_disc() * portal_radius;
                        const sim::named::SpawnParams params{e.elite_id, portal_pos + jitter,
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
