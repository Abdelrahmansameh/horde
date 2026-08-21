// game/telemetry/RunTelemetry.cpp — see the header for what this records and
// why. This file's one non-obvious job is turning per-tick cumulative state
// into per-wave differences, which is why so much of it is "remember what this
// counter read when the wave opened".
#include "game/telemetry/RunTelemetry.h"

#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace immune::game {
namespace {

using json = nlohmann::json;

/// Snake-case family names. Deliberately NOT ui::fmt::pathogen_family_name --
/// that one is title-cased for a HUD readout, and a report key that changes
/// when someone re-words a label is a report nothing can diff.
const char* family_key(u32 f) {
    static const char* kNames[kFamilyCount] = {"virus", "bacteria"};
    return f < kFamilyCount ? kNames[f] : "unknown";
}

const char* modifier_key(WaveModifier m) {
    switch (m) {
        case WaveModifier::None: return "none";
        case WaveModifier::Fever: return "fever";
        case WaveModifier::Swarm: return "swarm";
    }
    return "none";
}

f64 seconds(u64 ticks) { return static_cast<f64>(ticks) / static_cast<f64>(kTicksPerSecond); }

/// Guarded ratio: a tower that never fired divides by zero otherwise, and a
/// null in the report is honest where a 0.0 would rank it alongside a tower
/// that fired and achieved nothing.
json ratio(f64 numerator, f64 denominator) {
    if (denominator <= 0.0) return nullptr;
    return numerator / denominator;
}

json family_array(const f64* values) {
    json o = json::object();
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (values[f] != 0.0) o[family_key(f)] = values[f];
    }
    return o;
}

template <typename T>
json family_counts(const T* values) {
    json o = json::object();
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (values[f] != 0) o[family_key(f)] = values[f];
    }
    return o;
}

} // namespace

const char* run_result_name(RunResult r) {
    switch (r) {
        case RunResult::Cleared: return "cleared";
        case RunResult::Lost: return "lost";
        case RunResult::TickLimit: return "tick_limit";
        case RunResult::InProgress: return "in_progress";
    }
    return "in_progress";
}

void RunTelemetry::begin(sim::SimWorld& world, const LevelDef& level, const LaneOwnershipMap& lanes) {
    towers_.clear();
    waves_.clear();
    timeline_.clear();
    attribution_.clear();
    lanes_ = &lanes;
    result_ = RunResult::InProgress;
    last_tick_ = 0;
    wave_open_ = false;
    idle_atp_seconds_ = 0.0;
    peak_atp_ = 0.0;
    (void)level;   // Reserved for the header; geometry comes from `lanes`.

    // All four damage paths, or the ranking is a ranking of which tower
    // happens to use the paths that were hooked: fields (five roles), rounds
    // (the Gunner), granules (the NK Cell's blades), and the coverage grid
    // (the Goblet Cell's mucus).
    world.damage().set_attribution(&attribution_);
    world.projectile_system().set_attribution(&attribution_);
    world.swarmer_system().set_attribution(&attribution_);
    world.fluid_system().set_attribution(&attribution_);
}

void RunTelemetry::end(sim::SimWorld& world) {
    world.damage().set_attribution(nullptr);
    world.projectile_system().set_attribution(nullptr);
    world.swarmer_system().set_attribution(nullptr);
    world.fluid_system().set_attribution(nullptr);
}

TowerTelemetry* RunTelemetry::find(EntityId id) {
    // Reverse scan: EnTT recycles entity ids, so if one was reused after a
    // sell the LIVE record is the later of the two and the earlier one is
    // already closed. Searching from the back always finds the live one.
    for (auto it = towers_.rbegin(); it != towers_.rend(); ++it) {
        if (it->id == id) return &(*it);
    }
    return nullptr;
}

void RunTelemetry::on_tower_placed(EntityId id, TowerType type, Vec2 position, u32 cost, u64 tick) {
    // Close any record still open under a recycled id before opening the new
    // one, or the two towers' damage would be summed into one line.
    if (TowerTelemetry* prior = find(id); prior != nullptr && prior->removed_tick == 0) {
        prior->removed_tick = tick;
    }
    TowerTelemetry rec;
    rec.id = id;
    rec.type = type;
    rec.position = position;
    rec.built_tick = tick;
    rec.tier_tick[0] = tick;
    rec.invested_atp = cost;
    if (lanes_ != nullptr) {
        const i32 lane = lanes_->lane_at(position);
        if (lane >= 0 && static_cast<usize>(lane) < lanes_->lane_ids.size()) {
            rec.lane = lanes_->lane_ids[static_cast<usize>(lane)];
        }
    }
    towers_.push_back(std::move(rec));
}

void RunTelemetry::on_tower_upgraded(EntityId id, u8 new_tier, u32 cost, u64 tick) {
    TowerTelemetry* rec = find(id);
    if (rec == nullptr) return;
    rec->tier = new_tier;
    rec->peak_tier = std::max(rec->peak_tier, new_tier);
    rec->invested_atp += cost;
    if (new_tier >= 1 && new_tier <= 3) rec->tier_tick[new_tier - 1] = tick;
}

void RunTelemetry::on_tower_sold(EntityId id, u32 refund, u64 tick) {
    TowerTelemetry* rec = find(id);
    if (rec == nullptr) return;
    rec->removed_tick = tick;
    rec->refunded_atp = refund;
}

void RunTelemetry::close_wave(u64 tick) {
    if (!wave_open_ || waves_.empty()) return;
    WaveTelemetry& w = waves_.back();
    w.end_tick = tick;
    wave_open_ = false;
}

void RunTelemetry::sample(const sim::SimWorld& world, const WaveDirector& waves,
                          const Economy& economy) {
    const sim::SimSnapshot snap = world.snapshot();
    const WaveStatus status = waves.status();
    const EconomySnapshot econ = economy.snapshot();
    last_tick_ = snap.tick;

    // --- Per-tower: the sink is cumulative, so this overwrites rather than
    // accumulates. Overwriting is also what makes the pass idempotent if a
    // caller ever samples twice on one tick.
    for (const sim::AttributionEntry& e : attribution_.entries()) {
        TowerTelemetry* rec = find(e.owner);
        if (rec == nullptr) continue;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            rec->density_removed[f] = e.density_removed[f];
            rec->chaff_killed[f] = e.chaff_killed[f];
        }
        rec->named_damage = e.named_damage;
        rec->named_killed = e.named_killed;
        rec->active_ticks = e.active_ticks;
    }
    u32 live_towers = 0;
    for (TowerTelemetry& t : towers_) {
        if (t.removed_tick == 0) {
            ++t.alive_ticks;
            ++live_towers;
        }
    }

    // --- Economy pressure. ATP sitting in the bank is ATP the level's pricing
    // failed to give the player anything to want.
    idle_atp_seconds_ += static_cast<f64>(econ.atp) / static_cast<f64>(kTicksPerSecond);
    peak_atp_ = std::max(peak_atp_, static_cast<f64>(econ.atp));

    // --- Per-wave. A wave opens on the first tick its index is current and
    // closes when the index moves on or every wave completes.
    const bool index_changed = wave_open_ && status.wave_index != open_wave_index_;
    if (index_changed) close_wave(snap.tick);
    if (!wave_open_ && !status.all_waves_complete) {
        WaveTelemetry w;
        w.index = status.wave_index;
        if (status.wave_index < waves.waves().size()) {
            const WaveDef& def = waves.waves()[status.wave_index];
            w.name = def.name;
            w.modifier = modifier_key(def.modifier);
        }
        w.start_tick = snap.tick;
        w.integrity_at_start = snap.objective_integrity;
        w.atp_at_start = econ.atp;
        w.towers_at_start = live_towers;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            spawned_at_wave_start_[f] = snap.chaff_spawned_by_family[f];
            killed_at_wave_start_[f] = snap.chaff_killed_by_family[f];
            leaked_at_wave_start_[f] = snap.chaff_leaked_by_family[f];
        }
        earned_at_wave_start_ = econ.total_earned;
        spent_at_wave_start_ = econ.total_spent;
        waves_.push_back(std::move(w));
        wave_open_ = true;
        open_wave_index_ = status.wave_index;
    }

    if (wave_open_ && !waves_.empty()) {
        WaveTelemetry& w = waves_.back();
        switch (status.phase) {
            case WavePhase::Prep: ++w.prep_ticks; break;
            case WavePhase::Spawning: ++w.spawning_ticks; break;
            case WavePhase::Clearing: ++w.clearing_ticks; break;
            case WavePhase::Complete: break;
        }
        w.end_tick = snap.tick;
        w.integrity_at_end = snap.objective_integrity;
        w.atp_at_end = econ.atp;
        w.towers_at_end = live_towers;
        w.atp_earned = econ.total_earned - earned_at_wave_start_;
        w.atp_spent = econ.total_spent - spent_at_wave_start_;
        w.peak_chaff = std::max(w.peak_chaff, snap.chaff_count);
        w.peak_density = std::max(w.peak_density, snap.total_density);
        for (u32 f = 0; f < kFamilyCount; ++f) {
            w.spawned[f] = snap.chaff_spawned_by_family[f] - spawned_at_wave_start_[f];
            w.killed[f] = snap.chaff_killed_by_family[f] - killed_at_wave_start_[f];
            w.leaked[f] = snap.chaff_leaked_by_family[f] - leaked_at_wave_start_[f];
        }
    }
    if (status.all_waves_complete) close_wave(snap.tick);

    for (u32 f = 0; f < kFamilyCount; ++f) {
        final_spawned_[f] = snap.chaff_spawned_by_family[f];
        final_killed_[f] = snap.chaff_killed_by_family[f];
        final_leaked_[f] = snap.chaff_leaked_by_family[f];
        final_despawned_[f] = snap.chaff_despawned_by_family[f];
    }
    final_integrity_ = snap.objective_integrity;
    final_atp_ = econ.atp;
    total_earned_ = econ.total_earned;
    total_spent_ = econ.total_spent;

    // --- Timeline.
    if (snap.tick % timeline_interval_ == 0) {
        TimelineSample s;
        s.tick = snap.tick;
        s.wave_index = status.wave_index;
        s.phase = static_cast<u8>(status.phase);
        s.atp = econ.atp;
        s.total_earned = econ.total_earned;
        s.total_spent = econ.total_spent;
        s.chaff_count = snap.chaff_count;
        s.total_density = snap.total_density;
        s.integrity = snap.objective_integrity;
        s.towers = live_towers;
        timeline_.push_back(s);
    }
    last_phase_ = static_cast<u8>(status.phase);
}

std::string RunTelemetry::to_json(const ReportHeader& header, int indent) const {
    json doc;
    doc["schema"] = 1;
    doc["level"] = header.level_path;
    doc["level_name"] = header.level_name;
    doc["profile"] = header.profile;
    doc["seed"] = header.seed;
    doc["config_hash"] = header.config_hash;
    doc["config_dir"] = header.config_dir;
    doc["result"] = run_result_name(result_);
    doc["ticks"] = last_tick_;
    doc["seconds"] = seconds(last_tick_);
    doc["waves_reached"] = waves_.empty() ? 0u : waves_.back().index + 1u;

    // --- Per-tower instances.
    json towers = json::array();
    f64 type_atp[kTowerTypeCount] = {};
    f64 type_density[kTowerTypeCount] = {};
    f64 type_named[kTowerTypeCount] = {};
    u64 type_kills[kTowerTypeCount] = {};
    u64 type_active[kTowerTypeCount] = {};
    u64 type_alive[kTowerTypeCount] = {};
    u32 type_count[kTowerTypeCount] = {};
    for (const TowerTelemetry& t : towers_) {
        const f64 density = t.total_density_removed();
        json j;
        j["id"] = t.id.value;
        j["type"] = tower_type_name(t.type);
        j["tier"] = t.tier;
        j["peak_tier"] = t.peak_tier;
        j["position"] = {t.position.x, t.position.y};
        j["lane"] = t.lane;
        j["built_tick"] = t.built_tick;
        j["removed_tick"] = t.removed_tick;
        j["tier_ticks"] = {t.tier_tick[0], t.tier_tick[1], t.tier_tick[2]};
        j["invested_atp"] = t.invested_atp;
        j["refunded_atp"] = t.refunded_atp;
        j["density_removed"] = family_array(t.density_removed);
        j["density_removed_total"] = density;
        j["chaff_killed"] = family_counts(t.chaff_killed);
        j["named_damage"] = t.named_damage;
        j["named_killed"] = t.named_killed;
        j["alive_ticks"] = t.alive_ticks;
        j["active_ticks"] = t.active_ticks;
        // The three numbers a balance pass actually reads.
        j["atp_per_density"] = ratio(static_cast<f64>(t.invested_atp), density);
        j["density_per_second"] = ratio(density, seconds(t.alive_ticks));
        j["uptime"] = ratio(static_cast<f64>(t.active_ticks), static_cast<f64>(t.alive_ticks));
        towers.push_back(std::move(j));

        const u32 ti = static_cast<u32>(t.type);
        if (ti < kTowerTypeCount) {
            type_atp[ti] += t.invested_atp;
            type_density[ti] += density;
            type_named[ti] += t.named_damage;
            for (u32 f = 0; f < kFamilyCount; ++f) type_kills[ti] += t.chaff_killed[f];
            type_active[ti] += t.active_ticks;
            type_alive[ti] += t.alive_ticks;
            ++type_count[ti];
        }
    }
    doc["towers"] = std::move(towers);

    // --- Per tower TYPE: the ranking table. A type with no instances is
    // emitted with zeros rather than omitted, because "the bot never once
    // bought this tower" is itself the finding.
    json by_type = json::array();
    for (u32 ti = 0; ti < kTowerTypeCount; ++ti) {
        json j;
        j["type"] = tower_type_name(static_cast<TowerType>(ti));
        j["instances"] = type_count[ti];
        j["invested_atp"] = type_atp[ti];
        j["density_removed"] = type_density[ti];
        j["named_damage"] = type_named[ti];
        j["chaff_killed"] = type_kills[ti];
        j["atp_per_density"] = ratio(type_atp[ti], type_density[ti]);
        j["density_per_second"] = ratio(type_density[ti], seconds(type_alive[ti]));
        j["uptime"] = ratio(static_cast<f64>(type_active[ti]), static_cast<f64>(type_alive[ti]));
        by_type.push_back(std::move(j));
    }
    doc["towers_by_type"] = std::move(by_type);

    // --- Per wave.
    json waves = json::array();
    for (const WaveTelemetry& w : waves_) {
        json j;
        j["index"] = w.index;
        j["name"] = w.name;
        j["modifier"] = w.modifier;
        j["start_tick"] = w.start_tick;
        j["end_tick"] = w.end_tick;
        j["prep_seconds"] = seconds(w.prep_ticks);
        j["spawning_seconds"] = seconds(w.spawning_ticks);
        j["clearing_seconds"] = seconds(w.clearing_ticks);
        j["integrity_at_start"] = w.integrity_at_start;
        j["integrity_at_end"] = w.integrity_at_end;
        j["integrity_cost"] = w.integrity_at_start - w.integrity_at_end;
        j["atp_at_start"] = w.atp_at_start;
        j["atp_at_end"] = w.atp_at_end;
        j["atp_earned"] = w.atp_earned;
        j["atp_spent"] = w.atp_spent;
        j["towers_at_start"] = w.towers_at_start;
        j["towers_at_end"] = w.towers_at_end;
        j["spawned"] = family_counts(w.spawned);
        j["killed"] = family_counts(w.killed);
        j["leaked"] = family_counts(w.leaked);
        u64 spawned = 0, killed = 0, leaked = 0;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            spawned += w.spawned[f];
            killed += w.killed[f];
            leaked += w.leaked[f];
        }
        j["spawned_total"] = spawned;
        j["killed_total"] = killed;
        j["leaked_total"] = leaked;
        j["leak_rate"] = ratio(static_cast<f64>(leaked), static_cast<f64>(spawned));
        j["peak_chaff"] = w.peak_chaff;
        j["peak_density"] = w.peak_density;
        waves.push_back(std::move(j));
    }
    doc["waves"] = std::move(waves);

    // --- Timeline.
    json timeline = json::array();
    for (const TimelineSample& s : timeline_) {
        timeline.push_back(json{{"tick", s.tick},
                                {"wave", s.wave_index},
                                {"phase", s.phase},
                                {"atp", s.atp},
                                {"earned", s.total_earned},
                                {"spent", s.total_spent},
                                {"chaff", s.chaff_count},
                                {"density", s.total_density},
                                {"integrity", s.integrity},
                                {"towers", s.towers}});
    }
    doc["timeline"] = std::move(timeline);

    // --- Per family: what the level threw, and what became of it.
    json families = json::array();
    u64 all_spawned = 0, all_killed = 0, all_leaked = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (final_spawned_[f] == 0 && final_killed_[f] == 0 && final_leaked_[f] == 0) continue;
        json j;
        j["family"] = family_key(f);
        j["spawned"] = final_spawned_[f];
        j["killed"] = final_killed_[f];
        j["leaked"] = final_leaked_[f];
        j["despawned_out_of_bounds"] = final_despawned_[f];
        j["still_alive"] = final_spawned_[f] -
                           (final_killed_[f] + final_leaked_[f] + final_despawned_[f]);
        j["leak_rate"] = ratio(static_cast<f64>(final_leaked_[f]),
                               static_cast<f64>(final_spawned_[f]));
        // Density removed per family, summed across every tower: pairs with
        // leak_rate to say whether a family is surviving because it out-tanks
        // the towers or because nothing ever reached it.
        f64 density = 0.0;
        for (const TowerTelemetry& t : towers_) density += t.density_removed[f];
        j["density_removed"] = density;
        families.push_back(std::move(j));
        all_spawned += final_spawned_[f];
        all_killed += final_killed_[f];
        all_leaked += final_leaked_[f];
    }
    doc["families"] = std::move(families);
    doc["totals"] = json{{"spawned", all_spawned},
                         {"killed", all_killed},
                         {"leaked", all_leaked},
                         {"leak_rate", ratio(static_cast<f64>(all_leaked),
                                             static_cast<f64>(all_spawned))},
                         {"final_integrity", final_integrity_},
                         {"final_atp", final_atp_},
                         {"atp_earned", total_earned_},
                         {"atp_spent", total_spent_}};

    doc["economy"] = json{{"idle_atp_seconds", idle_atp_seconds_},
                          {"peak_atp", peak_atp_},
                          {"mean_banked_atp", ratio(idle_atp_seconds_, seconds(last_tick_))}};
    return doc.dump(indent);
}

} // namespace immune::game
