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

// ---------------------------------------------------------------------------
// Per-region wave tables (DESIGN.md §4.6, §6.5). generate() used to be a
// single flat escalation regardless of region string; each region now gets
// its own composition, matching the region table's "design role" column, so
// difficulty growth across regions reads as "a new thing to learn" rather
// than "the same thing, more of it" (§6.5). Unknown/unhandled region strings
// (including "", used by headless --bench/--sim-test paths with no real
// level) fall back to generate_flat(), the original region-agnostic body,
// verbatim -- see its own comment.
// ---------------------------------------------------------------------------

enum class RegionKind : u8 { Skin, Capillary, Lymphatic, Mucosal, OrganChamber, Unknown };

RegionKind classify_region(const std::string& region) {
    if (region == "skin" || region == "epidermis") return RegionKind::Skin;
    if (region == "capillary") return RegionKind::Capillary;
    if (region == "lymphatic") return RegionKind::Lymphatic;
    if (region == "mucosal") return RegionKind::Mucosal;
    if (region == "organ_chamber" || region == "organ-chamber") return RegionKind::OrganChamber;
    return RegionKind::Unknown;
}

/// DESIGN.md §4.5: "A generous first-wave prep window ... subsequent
/// inter-wave prep windows shrink slightly as the level progresses, pushing
/// the player from 'plan calmly' toward 'react decisively'" -- an explicit
/// per-level *curve*, long to progressively shorter, not a flat value. Linear
/// from `first` (wave 0) down to `floor` (the last wave); each region picks
/// its own generosity/floor but every region shares this same downward shape
/// (deliverable 3).
f32 prep_curve(u32 i, u32 wave_count, f32 first, f32 floor) {
    if (wave_count <= 1) return first;
    const f32 t = static_cast<f32>(i) / static_cast<f32>(wave_count - 1);
    return first - (first - floor) * t;
}

SpawnEntry make_spawn(PathogenFamily family, u32 count, f32 start_time, f32 duration) {
    SpawnEntry e;
    e.family = family;
    e.count = count;
    e.start_time = start_time;
    e.duration = duration;
    return e;
}

/// Region-agnostic fallback: virus, +bacteria from wave 2, +fungal from wave
/// 4, base count 120 + i*75, flat 8s/15s prep. This is the *original*
/// generate() body, kept byte-for-byte in shape so anything that generates
/// with an unrecognized region string (headless --bench scenarios, tests that
/// don't pass a real level region) doesn't regress.
std::vector<WaveDef> generate_flat(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = (i == 0) ? 8.0f : 15.0f;
        w.atp_reward = 50 + i * 10;

        const u32 base = 120 + i * 75;
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 30.0f)), 0.0f, 4.0f));
        if (i >= 1) w.spawns.push_back(make_spawn(PathogenFamily::Bacteria, base / 3, 1.0f, 3.34f));
        if (i >= 3) w.spawns.push_back(make_spawn(PathogenFamily::FungalSpore, base / 4, 0.67f, 5.34f));

        waves.push_back(std::move(w));
    }
    return waves;
}

/// Skin / epidermis (§4.6): onboarding. One lane, one family, short table,
/// gentle escalation -- "teach that obstacles cause crowding before teaching
/// splash-around", per the region table's fluid-behavior emphasis.
std::vector<WaveDef> generate_skin(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = prep_curve(i, wave_count, 12.0f, 9.0f);
        w.atp_reward = 40 + i * 8;

        const u32 base = 60 + i * 18;
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 12.0f)), 0.0f, 4.67f));

        waves.push_back(std::move(w));
    }
    return waves;
}

/// Capillary network (§4.6): chokepoint/precision teaching. Close to the
/// original flat table (it already matched this region's role reasonably
/// well) -- virus base, bacteria joining at wave 2, fungal spore drift
/// joining late as a light preview of the mucosal region -- but with a real
/// downward prep-time curve instead of the old flat-after-wave-1 value.
std::vector<WaveDef> generate_capillary(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = prep_curve(i, wave_count, 8.0f, 4.0f);
        w.atp_reward = 50 + i * 10;

        const u32 base = 120 + i * 75;
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 30.0f)), 0.0f, 4.0f));
        if (i >= 1) w.spawns.push_back(make_spawn(PathogenFamily::Bacteria, base / 3, 1.0f, 3.34f));
        if (i >= 3) w.spawns.push_back(make_spawn(PathogenFamily::FungalSpore, base / 4, 0.67f, 5.34f));

        waves.push_back(std::move(w));
    }
    return waves;
}

/// Lymphatic corridors (§4.6): "home turf", AoE/DoT teaching. Bacteria
/// clumping pressure appears from wave 1 (not wave 2, like capillary) and at
/// a heavier share of the wave every time -- the wide, slow-flow lane is
/// exactly where a biofilm clump reads clearly, per the region's design role.
std::vector<WaveDef> generate_lymphatic(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = prep_curve(i, wave_count, 9.0f, 5.0f);
        w.atp_reward = 55 + i * 12;

        const u32 base = 105 + i * 60;
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 24.0f)), 0.0f, 4.0f));
        w.spawns.push_back(make_spawn(PathogenFamily::Bacteria, base / 2, 0.5f, 4.0f)); // clumping, every wave
        if (i >= 4) w.spawns.push_back(make_spawn(PathogenFamily::FungalSpore, base / 5, 1.0f, 4.67f));

        waves.push_back(std::move(w));
    }
    return waves;
}

/// Mucosal surfaces (§4.6): floodplain, agent count peaks here. Fungal
/// spore's wide lateral drift is the star from wave 1 -- "full fluid
/// spectacle at agent-count peak" is this region's entire design role.
std::vector<WaveDef> generate_mucosal(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = prep_curve(i, wave_count, 9.0f, 5.0f);
        w.atp_reward = 60 + i * 15;

        const u32 base = 180 + i * 105; // highest agent counts of any region
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 36.0f)), 0.0f, 4.67f));
        w.spawns.push_back(make_spawn(PathogenFamily::FungalSpore, base / 2, 0.33f, 6.0f)); // drift, every wave
        if (i >= 1) w.spawns.push_back(make_spawn(PathogenFamily::Bacteria, base / 3, 0.83f, 4.0f));

        waves.push_back(std::move(w));
    }
    return waves;
}

/// Organ chamber (§4.6): region finale. Longest table, most family variety
/// (every family present from wave 0), and the final wave is explicitly
/// boosted in both count and an added WaveModifier -- no boss entity exists
/// yet (that's Wave 5B's job), so per §4.5's rule ("the final wave is always
/// the hardest wave of the level") this is how the table ends escalated
/// without one.
std::vector<WaveDef> generate_organ_chamber(const std::string& region, u32 wave_count, Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region + "_wave_" + std::to_string(i + 1);
        w.prep_time = prep_curve(i, wave_count, 10.0f, 4.0f);

        const bool is_final = (i + 1 == wave_count);
        const f32 final_mult = is_final ? 1.75f : 1.0f;
        w.atp_reward = static_cast<u32>(static_cast<f32>(70 + i * 15) * (is_final ? 1.5f : 1.0f));

        const u32 base = static_cast<u32>(static_cast<f32>(135 + i * 66) * final_mult);
        w.spawns.push_back(make_spawn(PathogenFamily::Virus,
                                       base + static_cast<u32>(rng.range_f(0.0f, 36.0f)), 0.0f, 4.0f));
        w.spawns.push_back(make_spawn(PathogenFamily::Bacteria, base / 2, 0.5f, 4.0f));
        w.spawns.push_back(make_spawn(PathogenFamily::FungalSpore, base / 3, 0.83f, 4.67f));
        if (is_final) w.modifier = WaveModifier::Swarm; // §6 allergen/curveball framework's Swarm case

        waves.push_back(std::move(w));
    }
    return waves;
}

} // namespace

void WaveDirector::set_waves(std::vector<WaveDef> waves) { waves_ = std::move(waves); }

std::vector<WaveDef> WaveDirector::generate(const std::string& region, u32 wave_count, Rng& rng) {
    switch (classify_region(region)) {
        case RegionKind::Skin: return generate_skin(region, wave_count, rng);
        case RegionKind::Capillary: return generate_capillary(region, wave_count, rng);
        case RegionKind::Lymphatic: return generate_lymphatic(region, wave_count, rng);
        case RegionKind::Mucosal: return generate_mucosal(region, wave_count, rng);
        case RegionKind::OrganChamber: return generate_organ_chamber(region, wave_count, rng);
        case RegionKind::Unknown:
        default:
            return generate_flat(region, wave_count, rng);
    }
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
