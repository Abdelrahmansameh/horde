// game/wave/WaveDirector.cpp — implementation of the frozen WaveDirector.h
// contract. Owner: Wave 3A (full per-region tables/tuning); this is the real,
// permanent Prep->Spawning->Clearing->Complete state machine and a minimal
// but genuine generate(), not a placeholder.
#include "game/wave/WaveDirector.h"

#include "game/wave/WaveConfigApply.h"

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

/// One generator, driven by a region entry from assets/config/waves.json.
///
/// This replaced five near-identical hand-written generators. They differed
/// only in their prep curve, their reward and count ramps, which families
/// joined at which wave and at what share, and whether the final wave was
/// boosted -- every one of which is now a field, so a region's whole pressure
/// curve is editable without touching this file.
/// Canonical config key for a level's region string, via the same classifier
/// the hand-written generators used to switch on.
const char* canonical_region_name(const std::string& region) {
    switch (classify_region(region)) {
        case RegionKind::Skin: return "skin";
        case RegionKind::Capillary: return "capillary";
        case RegionKind::Lymphatic: return "lymphatic";
        case RegionKind::Mucosal: return "mucosal";
        case RegionKind::OrganChamber: return "organ_chamber";
        case RegionKind::Unknown: break;
    }
    return "flat";
}

std::vector<WaveDef> generate_from_region(const std::string& region_name,
                                          const WaveRegionConfig& region, u32 wave_count,
                                          Rng& rng) {
    std::vector<WaveDef> waves;
    waves.reserve(wave_count);
    const WaveRegionScaling& sc = region.scaling;

    for (u32 i = 0; i < wave_count; ++i) {
        WaveDef w;
        w.index = i;
        w.name = region_name + "_wave_" + std::to_string(i + 1);
        w.prep_time = region.prep_mode == PrepMode::Curve
                          ? prep_curve(i, wave_count, sc.prep_first, sc.prep_floor)
                          : ((i == 0) ? sc.prep_first : sc.prep_floor);

        const bool is_final = (i + 1 == wave_count);
        const f32 count_mul = is_final ? sc.final_count_mul : 1.0f;
        const f32 reward_mul = is_final ? sc.final_reward_mul : 1.0f;

        w.atp_reward = static_cast<u32>(
            static_cast<f32>(sc.atp_base + i * sc.atp_per_wave) * reward_mul);
        const u32 base = static_cast<u32>(
            static_cast<f32>(sc.count_base + i * sc.count_per_wave) * count_mul);

        for (const WaveTrackConfig& track : region.tracks) {
            if (i < track.from_wave) continue;
            // The RNG draw is taken ONLY for tracks that declare jitter, so
            // adding a jitter-free track to a region cannot shift the stream
            // for the tracks after it.
            u32 count = base / (track.count_divisor == 0 ? 1u : track.count_divisor);
            if (track.count_jitter > 0.0f) {
                count += static_cast<u32>(rng.range_f(0.0f, track.count_jitter));
            }
            w.spawns.push_back(
                make_spawn(track.family, count, track.start_time, track.duration));
        }

        if (is_final) w.modifier = region.final_modifier;
        waves.push_back(std::move(w));
    }
    return waves;
}

} // namespace

void WaveDirector::set_waves(std::vector<WaveDef> waves) { waves_ = std::move(waves); }

std::vector<WaveDef> WaveDirector::generate(const std::string& region, u32 wave_count, Rng& rng) {
    // classify_region() maps a level's region string onto a canonical name;
    // anything unrecognized lands on "flat", which waves.json requires to
    // exist for exactly that reason.
    const WaveRegionConfig* entry = wave_config().find_region(canonical_region_name(region));
    if (entry == nullptr) entry = wave_config().find_region("flat");
    if (entry == nullptr) return {};
    return generate_from_region(region, *entry, wave_count, rng);
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

namespace {

WaveTrackConfig make_track(PathogenFamily family, u32 from_wave, u32 divisor, f32 jitter,
                           f32 start, f32 duration) {
    WaveTrackConfig t;
    t.family = family;
    t.from_wave = from_wave;
    t.count_divisor = divisor;
    t.count_jitter = jitter;
    t.start_time = start;
    t.duration = duration;
    return t;
}

WaveRegionConfig make_region(const char* name, PrepMode mode, f32 prep_first, f32 prep_floor,
                             u32 atp_base, u32 atp_per, u32 count_base, u32 count_per,
                             std::vector<WaveTrackConfig> tracks) {
    WaveRegionConfig r;
    r.name = name;
    r.prep_mode = mode;
    r.scaling.prep_first = prep_first;
    r.scaling.prep_floor = prep_floor;
    r.scaling.atp_base = atp_base;
    r.scaling.atp_per_wave = atp_per;
    r.scaling.count_base = count_base;
    r.scaling.count_per_wave = count_per;
    r.tracks = std::move(tracks);
    return r;
}

/// The five region curves (DESIGN.md 4.6) plus the region-agnostic fallback,
/// as they were written when each had its own generator function.
WaveConfig build_default_wave_config() {
    using PF = PathogenFamily;
    WaveConfig cfg;
    cfg.globals = WaveGlobals{8u, 60.0f, 0.75f};

    // Fallback for an unrecognized region string (headless --bench scenarios,
    // tests that pass no real level). Keeps the original flat shape: a
    // generous first window, then a fixed one, rather than a curve.
    cfg.regions.push_back(make_region("flat", PrepMode::FirstThenFlat, 8.0f, 15.0f, 50, 10, 120, 75,
                                      {make_track(PF::Virus, 0, 1, 30.0f, 0.0f, 4.0f),
                                       make_track(PF::Bacteria, 1, 3, 0.0f, 1.0f, 3.34f),
                                       make_track(PF::FungalSpore, 3, 4, 0.0f, 0.67f, 5.34f)}));

    // Skin: onboarding. One lane, one family, gentle escalation.
    cfg.regions.push_back(make_region("skin", PrepMode::Curve, 12.0f, 9.0f, 40, 8, 60, 18,
                                      {make_track(PF::Virus, 0, 1, 12.0f, 0.0f, 4.67f)}));

    // Capillary: chokepoint/precision teaching, with fungal drift late as a
    // preview of the mucosal region.
    cfg.regions.push_back(make_region("capillary", PrepMode::Curve, 8.0f, 4.0f, 50, 10, 120, 75,
                                      {make_track(PF::Virus, 0, 1, 30.0f, 0.0f, 4.0f),
                                       make_track(PF::Bacteria, 1, 3, 0.0f, 1.0f, 3.34f),
                                       make_track(PF::FungalSpore, 3, 4, 0.0f, 0.67f, 5.34f)}));

    // Lymphatic: home turf, AoE/DoT teaching. Bacteria clump from wave 0 and
    // at a heavier share -- the wide slow lane is where a biofilm reads.
    cfg.regions.push_back(make_region("lymphatic", PrepMode::Curve, 9.0f, 5.0f, 55, 12, 105, 60,
                                      {make_track(PF::Virus, 0, 1, 24.0f, 0.0f, 4.0f),
                                       make_track(PF::Bacteria, 0, 2, 0.0f, 0.5f, 4.0f),
                                       make_track(PF::FungalSpore, 4, 5, 0.0f, 1.0f, 4.67f)}));

    // Mucosal: floodplain, agent count peaks here. Fungal lateral drift is the
    // star from wave 0.
    cfg.regions.push_back(make_region("mucosal", PrepMode::Curve, 9.0f, 5.0f, 60, 15, 180, 105,
                                      {make_track(PF::Virus, 0, 1, 36.0f, 0.0f, 4.67f),
                                       make_track(PF::FungalSpore, 0, 2, 0.0f, 0.33f, 6.0f),
                                       make_track(PF::Bacteria, 1, 3, 0.0f, 0.83f, 4.0f)}));

    // Organ chamber: region finale. Every family from wave 0, and the last
    // wave escalated in count, reward and modifier -- no boss entity exists for
    // it yet, so this is how the table ends hardest.
    WaveRegionConfig organ = make_region("organ_chamber", PrepMode::Curve, 10.0f, 4.0f, 70, 15,
                                         135, 66,
                                         {make_track(PF::Virus, 0, 1, 36.0f, 0.0f, 4.0f),
                                          make_track(PF::Bacteria, 0, 2, 0.0f, 0.5f, 4.0f),
                                          make_track(PF::FungalSpore, 0, 3, 0.0f, 0.83f, 4.67f)});
    organ.scaling.final_count_mul = 1.75f;
    organ.scaling.final_reward_mul = 1.5f;
    organ.final_modifier = WaveModifier::Swarm;
    cfg.regions.push_back(std::move(organ));

    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Config application (game/wave/WaveConfigApply.h)
// ---------------------------------------------------------------------------

namespace {

/// Holds the five region curves this file used to hardcode, so a generate()
/// call that happens before any config is loaded produces the same table it
/// always did -- and so default_game_config() reads them back out of here.
WaveConfig& mutable_wave_config() {
    static WaveConfig cfg = build_default_wave_config();
    return cfg;
}

} // namespace

const WaveConfig& wave_config() { return mutable_wave_config(); }

void apply_wave_config(const WaveConfig& cfg) { mutable_wave_config() = cfg; }

} // namespace immune::game
