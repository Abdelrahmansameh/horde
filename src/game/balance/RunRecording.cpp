#include "game/balance/RunRecording.h"

#include "game/balance/BalanceRun.h"
#include "game/economy/Economy.h"
#include "game/level/LevelWriter.h"
#include "game/session/RunSetup.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace immune::game::balance {
namespace {
using json = config::Json;
constexpr const char* family_names[] = {"virus", "bacteria", "parasite"};
constexpr const char* ability_keys[] = {"complement", "histamine", "fever", "clot"};

json observed_economy(const Economy& economy) {
    const auto e = economy.snapshot();
    return {{"atp", e.atp}, {"opening_atp", e.opening_atp},
            {"damage_income", e.damage_income}, {"wave_income", e.bounty_income},
            {"refunds", e.refunds}, {"spent", e.total_spent},
            {"fractional_income", e.fractional_income}};
}

json observed_wave(const WaveDirector& waves) {
    const auto w = waves.status();
    return {{"wave_index", w.wave_index}, {"phase", static_cast<u32>(w.phase)},
            {"phase_time_remaining", w.phase_time_remaining},
            {"remaining_to_spawn", w.remaining_to_spawn},
            {"all_waves_complete", w.all_waves_complete}};
}

json observed_state(const sim::SimWorld& world) {
    const auto s = world.snapshot();
    json families = json::object();
    u64 kills = 0, leaks = 0, despawns = 0, spawned = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        families[family_names[f]] = {{"spawned", s.chaff_spawned_by_family[f]},
            {"killed", s.chaff_killed_by_family[f]}, {"leaked", s.chaff_leaked_by_family[f]},
            {"despawned", s.chaff_despawned_by_family[f]}, {"alive", s.chaff_by_family[f]}};
        kills += s.chaff_killed_by_family[f];
        leaks += s.chaff_leaked_by_family[f];
        despawns += s.chaff_despawned_by_family[f];
        spawned += s.chaff_spawned_by_family[f];
    }
    u64 friendly_alive = 0;
    const auto& swarm = world.swarmers();
    for (usize i = 0; i < swarm.count(); ++i) {
        if (swarm.health[i] > 0.0f && (swarm.flags[i] & sim::swarmer_flags::kPendingKill) == 0) ++friendly_alive;
    }
    return {{"tick", s.tick}, {"alive", s.chaff_count}, {"density", s.total_density},
        {"integrity", s.objective_integrity}, {"killed", kills}, {"leaked", leaks},
        {"despawned", despawns}, {"spawned", spawned}, {"families", families},
        {"friendly_alive", friendly_alive}, {"friendly_killed", s.swarmers_killed_total},
        {"scars_alive", s.scars_live}, {"scars_built", s.scars_built_total},
        {"scars_lost", s.scars_lost_total}, {"burrowed", s.chaff_burrowed}};
}

json observed_damage(const sim::DensityMetrics& m) {
    return {{"attempted_density", m.attempted_density}, {"density_removed", m.density_removed},
        {"overkill_density", m.overkill_density}, {"immune_density", m.immune_density},
        {"damage_kills", m.damage_kills}, {"damage_calls", m.damage_calls}};
}

json observed_distribution(const sim::TickDistribution& d) {
    // Raw histogram observations are the comparison contract. Quantiles can
    // be derived later without making a recording depend on display rounding.
    return {{"observations", d.observations}, {"sum_ticks", d.sum_ticks},
        {"min_ticks", d.min_ticks}, {"max_ticks", d.max_ticks}, {"bins", d.bins}};
}

json observed_lifecycle(const sim::LifecycleMetrics& m) {
    return {{"spawned", m.spawned}, {"entered_alive", m.entered_alive}, {"alive", m.alive},
        {"hit_alive", m.hit_alive}, {"first_hits", m.first_hits},
        {"damage_deaths", m.damage_deaths}, {"leaked", m.leaked},
        {"out_of_bounds", m.out_of_bounds}, {"consumed", m.consumed}, {"expired", m.expired},
        {"unknown", m.unknown}, {"damage_taken", m.damage_taken},
        {"damage_ttk", observed_distribution(m.damage_ttk)},
        {"damage_lifetime", observed_distribution(m.damage_lifetime)},
        {"retired_lifetime", observed_distribution(m.retired_lifetime)}};
}

json observed_combat(const sim::CombatMetrics& metrics) {
    const auto& t = metrics.totals();
    json result = {{"enabled", metrics.enabled()}, {"overflow", metrics.overflow_count()},
        {"damage", observed_damage(t.damage)},
        {"hostile_requested_cell_damage", t.hostile_requested_cell_damage},
        {"hostile_actual_cell_damage", t.hostile_actual_cell_damage},
        {"hostile_requested_structure_damage", t.hostile_requested_structure_damage},
        {"cells_killed_by_hostiles", t.cells_killed_by_hostiles},
        {"toxin_fired", t.toxin_fired}, {"toxin_hits", t.toxin_hits}, {"new_latches", t.new_latches},
        {"replications_denied_capacity", t.replications_denied_capacity},
        {"replications_denied_tick_budget", t.replications_denied_tick_budget},
        {"friendly_lifetimes", json::array()}};
    for (u32 f = 0; f < kFamilyCount; ++f) {
        result["families"][family_names[f]] = observed_damage(t.by_family[f]);
        result["enemy_lifetimes"][family_names[f]] = observed_lifecycle(metrics.enemy_lifetimes()[f]);
    }
    for (const auto& friendly : metrics.friendly_lifetimes()) {
        result["friendly_lifetimes"].push_back({{"owner", friendly.owner.value},
            {"type", tower_type_name(friendly.type)}, {"stats", observed_lifecycle(friendly.stats)}});
    }
    return result;
}

void compare_selected(const json& expected, const json& actual, const std::string& path,
                      std::vector<std::string>& errors) {
    if (expected.is_object()) {
        if (!actual.is_object()) { errors.push_back(path + ": expected object"); return; }
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            const std::string child = path.empty() ? it.key() : path + "." + it.key();
            if (!actual.contains(it.key())) errors.push_back(child + ": missing");
            else compare_selected(it.value(), actual.at(it.key()), child, errors);
        }
    } else if (expected.is_array()) {
        if (!actual.is_array() || expected.size() != actual.size()) {
            errors.push_back(path + ": array size differs"); return;
        }
        for (usize i = 0; i < expected.size(); ++i)
            compare_selected(expected[i], actual[i], path + "[" + std::to_string(i) + "]", errors);
    } else if (expected != actual) {
        errors.push_back(path + ": expected " + expected.dump() + ", got " + actual.dump());
    }
}
} // namespace

void RunRecording::begin(const LevelDef& level, const GameConfig& base,
                          const MetaProgression& meta, bool sandbox,
                          const config::Json& level_document) {
    active_ = true;
    sandbox_ = sandbox;
    level_name_ = level.name;
    base_ = base;
    initial_meta_ = meta;
    const bool full_precision = level_document.is_object() && !level_document.empty();
    const json document = full_precision ? level_document : json::parse(level_to_json(level));
    recording_ = {{"schema", 1}, {"kind", "recording"}, {"replayable", true},
        {"full_precision", full_precision}, {"base_config", dump_game_config(base)},
        {"timeline", json::array()}, {"diagnostics", {{"errors", json::array()}, {"warnings", json::array()}}},
        {"spec", {{"schema", 1}, {"name", "Player recording: " + level.name},
            {"level_document", document}, {"actions", json::array()},
            {"progression", {{"profile", sandbox ? "all_unlocked" : "campaign"}}},
            {"progression_snapshot", json::parse(meta.to_json())},
            {"controller", {{"policy", "none"}, {"use_abilities", false}}},
            {"auto_start", false}, {"combat_metrics", true},
            {"deterministic_rebakes", true}, {"sample_ticks", 60}}}};
    recording_["provenance"] = {{"base_config_hash", fingerprint(json(dump_game_config(base)).dump())},
        {"level_hash", fingerprint(document.dump())}, {"threads", 1},
        {"deterministic_rebakes", true}, {"full_precision", full_precision}};
    if (!full_precision) recording_["diagnostics"]["warnings"].push_back(
        "LevelDef fallback uses three-decimal canonical serialization; exact original document was not supplied");
}

void RunRecording::append(const Action& action) {
    if (!active_) return;
    json item = {{"tick", action.tick}, {"kind", action_kind_name(action.kind)}};
    if (action.kind == ActionKind::Deploy) {
        if (action.quantity == 0) return;
        item["tower"] = tower_type_name(action.tower);
        item["quantity"] = action.quantity;
        item["position"] = {action.position.x, action.position.y};
    } else if (action.kind == ActionKind::CastAbility) {
        const u32 ability = static_cast<u32>(action.ability);
        if (ability >= kAbilityCount) {
            recording_["diagnostics"]["errors"].push_back("accepted action contained an unknown ability");
            return;
        }
        item["ability"] = ability_keys[ability];
        item["position"] = {action.position.x, action.position.y};
    }
    recording_["spec"]["actions"].push_back(std::move(item));
}

void RunRecording::sample(const sim::SimWorld& world, const Economy& economy,
                           const WaveDirector& waves) {
    if (!active_) return;
    recording_["spec"]["seed"] = world.desc().seed;
    json observation = observed_state(world);
    observation["state_hash"] = world.state_hash();
    observation["economy"] = observed_economy(economy);
    observation["wave_status"] = observed_wave(waves);
    auto& timeline = recording_["timeline"];
    if (!timeline.empty() && timeline.back().at("tick").get<Tick>() == world.tick_index())
        timeline.back() = std::move(observation);
    else timeline.push_back(std::move(observation));
}

config::Json RunRecording::finish(const sim::SimWorld& world, const Economy& economy,
                                  const WaveDirector& waves, SessionOutcome outcome) {
    if (!active_) return recording_;
    sample(world, economy, waves);
    const Tick ticks = world.tick_index();
    const char* status = outcome == SessionOutcome::Cleared ? "cleared" :
        outcome == SessionOutcome::ObjectiveDestroyed ? "defeated" : "aborted";
    recording_["spec"]["max_ticks"] = ticks;
    recording_["expected"] = {{"status", status}, {"tick", ticks},
        {"state_hash", world.state_hash()}, {"state", observed_state(world)},
        {"economy", observed_economy(economy)}, {"wave_status", observed_wave(waves)},
        {"combat", observed_combat(world.combat_metrics())}};
    RunReward reward;
    if (!sandbox_ && outcome != SessionOutcome::InProgress) {
        const auto result = collect_run_result(world, waves, outcome == SessionOutcome::Cleared);
        reward = MetaProgression::reward_for_run(result, base_.meta, !initial_meta_.level_cleared(level_name_));
    }
    recording_["expected"]["reward"] = {{"memory_cells", reward.memory_cells},
        {"antibodies", reward.antibodies}, {"first_clear", reward.first_clear}};
    auto& actions = recording_["spec"]["actions"];
    std::stable_sort(actions.begin(), actions.end(), [](const json& a, const json& b) {
        return a.at("tick").get<Tick>() < b.at("tick").get<Tick>();
    });
    auto& errors = recording_["diagnostics"]["errors"];
    if (ticks == 0) errors.push_back("recording stopped before the first simulation tick");
    if (ticks > 2160000) errors.push_back("recording exceeds the runner's supported tick bound");
    for (const auto& action : actions) {
        if (action.at("tick").get<Tick>() >= ticks) {
            errors.push_back("accepted action occurred at or after the final observation tick");
            break;
        }
    }
    if (!world.desc().deterministic_rebakes) errors.push_back("session did not use deterministic flow rebakes");
    if (!world.combat_metrics().enabled()) errors.push_back("session did not enable combat observation");
    if (world.combat_metrics().overflow_count() > 0) errors.push_back("combat observation capacity exceeded");
    recording_["replayable"] = errors.empty();
    active_ = false;
    return recording_;
}

bool recording_matches(const config::Json& recording, const config::Json& report,
                       std::vector<std::string>& errors) {
    errors.clear();
    try {
    if (recording.value("schema", 0) != 1 ||
        recording.value("kind", std::string()) != "recording" || !recording.contains("expected")) {
        errors.push_back("input is not a completed player recording");
        return false;
    }
    if (!recording.value("replayable", false)) {
        errors.push_back("recording is explicitly marked as not replayable");
        return false;
    }
    const auto& expected = recording.at("expected");
    const std::string observed_status = expected.at("status").get<std::string>();
    const std::string replay_status = report.value("summary", json::object()).value("status", std::string("missing"));
    const std::string required_status = observed_status == "aborted" ? "timeout" : observed_status;
    if (replay_status != required_status)
        errors.push_back("summary.status: expected " + required_status + ", got " + replay_status);
    for (const char* key : {"state_hash", "state", "economy", "wave_status", "combat", "reward"}) {
        if (!report.contains(key)) errors.push_back(std::string(key) + ": missing");
        else compare_selected(expected.at(key), report.at(key), key, errors);
    }
    if (!report.contains("summary") || !report.at("summary").contains("duration_ticks"))
        errors.push_back("summary.duration_ticks: missing");
    else compare_selected(expected.at("tick"), report.at("summary").at("duration_ticks"),
                          "summary.duration_ticks", errors);
    return errors.empty();
    } catch (const std::exception& error) {
        errors.push_back(std::string("invalid recording/report: ") + error.what());
        return false;
    }
}

} // namespace immune::game::balance
