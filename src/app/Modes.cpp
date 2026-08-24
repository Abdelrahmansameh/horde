#include "app/Modes.h"

#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include "core/Rng.h"
#include "game/abilities/AbilityConfigApply.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "vfx/Particles.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"
#include "sim/ecs/NamedAgents.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>

namespace immune::app {
namespace {

using json = nlohmann::json;

/// Builds the sim for a headless run: level geometry (from --level or the
/// built-in test level) plus deterministic tuning.
/// `out_lane_map`, when non-null, receives the per-cell lane attribution the
/// tissue pass needs for DESIGN.md §9.2's per-lane hue. Optional because only
/// --screenshot renders anything a human looks at; --bench and --sim-test have
/// no use for it and should not pay to build it.
bool build_world(sim::SimWorld& world, const Options& opt, usize max_chaff,
                 JobSystem* jobs, std::string& error,
                 game::LaneOwnershipMap* out_lane_map = nullptr) {
    game::LevelDef level;
    game::LevelLoader loader;

    if (!opt.level.empty() && platform::file_exists(opt.level)) {
        const auto res = loader.load_file(opt.level, level);
        if (!res.ok) {
            IMMUNE_LOG_WARN("level load failed (%s); falling back to the built-in test level",
                            res.error.c_str());
            level = game::LevelLoader::default_test_level();
        }
    } else {
        if (!opt.level.empty()) {
            IMMUNE_LOG_WARN("level '%s' not found; using the built-in test level",
                            opt.level.c_str());
        }
        level = game::LevelLoader::default_test_level();
    }

    sim::SimDesc desc;
    desc.seed = opt.seed;
    desc.max_chaff = max_chaff;
    desc.world_bounds = level.world_bounds;
    // Cell size ~= 2x the default separation radius, per SpatialHashDesc.
    desc.spatial_cell_size = 4.0f;
    // Without this, chaff runs on all-default ChaffFamilyParams: no viral
    // replication, generic speed for every family. The
    // roster is the single source of truth so the sim kernel and the roster
    // table can never disagree (EnemyRoster.h's own rationale).
    game::EnemyRoster roster;
    {
        // The headless paths must run on the same tuning the game does, or a
        // passing --sim-test proves nothing about what a player sees.
        std::string cfg_err;
        config::ConfigStore store;
        game::GameConfig cfg;
        if (game::load_game_config(store, resolve_config_dir(opt), cfg, cfg_err)) {
            game::apply_enemy_config(roster, cfg.enemies);
            desc.max_damage_fields = cfg.sim.capacities.max_damage_fields;
            desc.max_projectiles = cfg.sim.capacities.max_projectiles;
            desc.max_swarmers = cfg.sim.capacities.max_swarmers;
            desc.max_fluid_particles = cfg.sim.capacities.max_fluid_particles;
            desc.max_combat_events = cfg.sim.capacities.max_combat_events;
            desc.fluid_tuning = cfg.sim.fluid;
            desc.squad_tuning = cfg.sim.squads;
            desc.spatial_cell_size = cfg.sim.globals.spatial_cell_size;
            desc.flow_rebake_budget_ms = cfg.sim.globals.flow_rebake_budget_ms;
            desc.flow_smoothing_radius = cfg.sim.globals.flow_smoothing_radius;
            desc.flow_wall_cost = cfg.sim.globals.flow_wall_cost;
            desc.flow_wall_falloff = cfg.sim.globals.flow_wall_falloff;
            desc.flow_wall_exponent = cfg.sim.globals.flow_wall_exponent;
            roster.apply_to_tuning(desc.chaff_tuning);
            desc.chaff_tuning.max_replications_per_tick = cfg.sim.globals.max_replications_per_tick;
            desc.chaff_tuning.max_neighbors_sampled = cfg.sim.globals.max_neighbors_sampled;
            desc.chaff_tuning.ambient_drift = cfg.sim.globals.ambient_drift;
        } else {
            IMMUNE_LOG_ERROR("config load failed: %s", cfg_err.c_str());
            error = cfg_err;
            return false;
        }
    }
    if (level.ambient_drift != Vec2{0.0f, 0.0f}) {
        desc.chaff_tuning.ambient_drift = level.ambient_drift;
    }
    world.init(desc, jobs);

    const auto res = loader.instantiate(level, world);
    if (!res.ok) {
        error = res.error;
        return false;
    }
    // Enemy behaviour systems need their
    // systems registered to run at all. Safe to call from every headless
    // mode: idempotent per EnemyRoster's own design, and a no-op if nothing
    // ever spawns an elite.
    roster.register_systems(world);
    if (out_lane_map != nullptr) *out_lane_map = loader.build_lane_ownership_map(level);
    return true;
}

/// Unpacks a lane map plus the live flow field into the renderer's game/-free
/// view of them. Mirrors what App::render does for the interactive path, so a
/// --screenshot capture frames the same substrate the player sees.
render::TissueDecor tissue_decor(const sim::SimWorld& world,
                                 const game::LaneOwnershipMap& lanes) {
    render::TissueDecor decor;
    decor.flow = &world.flow();
    if (!lanes.owner.empty() && !lanes.lane_types.empty()) {
        decor.lane_owner = lanes.owner.data();
        decor.lane_type = reinterpret_cast<const u8*>(lanes.lane_types.data());
        decor.lane_count = static_cast<u32>(lanes.lane_types.size());
        decor.lane_width = lanes.width;
        decor.lane_height = lanes.height;
    }
    return decor;
}

/// Populates a world for a bench scenario at t=0.
void populate_scenario(sim::SimWorld& world, const BenchScenario& s) {
    // Named agents: owned by Wave 1D's sim/ecs module. install() registers
    // the named-agent systems and spawn_bench_population() scatters the
    // placeholder elite deterministically across the world bounds.
    if (s.named_count > 0) {
        sim::named::setup_bench_scenario(world, s.named_count);
    }

    // Damage fields: `s.damage_fields` was recorded into the bench-JSON
    // metadata but never actually submitted (found by Wave 4G verifying its
    // own VFX work -- no --screenshot-reachable scenario ever exercised
    // submit_fields() with live data). Scattered Circle fields, deterministic
    // via world.rng(). A huge lifetime rather than <=0 ("persistent") on
    // purpose: DamageField.h requires persistent fields be *resubmitted every
    // tick by their owner*, but populate_scenario() only runs once at t=0 --
    // nothing here ticks to resubmit them. A field that outlives any
    // realistic bench/screenshot run behaves identically for this purpose
    // without violating that contract.
    const Rect b = world.desc().world_bounds;
    Rng& rng = world.rng();
    for (u32 i = 0; i < s.damage_fields; ++i) {
        sim::DamageField field;
        field.shape = sim::FieldShape::Circle;
        field.origin = Vec2{rng.range_f(b.min.x, b.max.x), rng.range_f(b.min.y, b.max.y)};
        field.radius = rng.range_f(6.0f, 14.0f);
        field.kill_rate = rng.range_f(8.0f, 20.0f);
        field.falloff = 1.0f;
        field.lifetime = 1.0e6f;
        world.damage().submit(field);
    }

    if (s.chaff_count == 0) return;
    // Spread agents across the world rect so the spatial hash sees realistic
    // occupancy rather than one degenerate cell.
    for (u32 i = 0; i < s.chaff_count; ++i) {
        if (world.chaff().full()) break;
        sim::ChaffSpawnParams p;
        p.position = Vec2{rng.range_f(b.min.x, b.max.x), rng.range_f(b.min.y, b.max.y)};
        p.velocity = rng.unit_disc() * 2.0f;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        p.density = 1.0f;
        world.chaff().spawn(p);
    }
}

/// Snake-case family names, in PathogenFamily order. Shared by the per-family
/// snapshot keys below and by the metric names read_metric() accepts, so a
/// script's assertion and the report it reads spell a family identically.
const char* family_key(u32 f) {
    static const char* kNames[kFamilyCount] = {"virus", "bacteria"};
    return f < kFamilyCount ? kNames[f] : "unknown";
}

json snapshot_to_json(const sim::SimSnapshot& s) {
    json j;
    j["tick"] = s.tick;
    j["chaff_count"] = s.chaff_count;
    j["named_count"] = s.named_count;
    j["total_density"] = s.total_density;
    j["objective_integrity"] = s.objective_integrity;
    j["chaff_killed_total"] = s.chaff_killed_total;
    j["chaff_leaked_total"] = s.chaff_leaked_total;
    // Per family. `killed` here means killed by damage; leaks and
    // out-of-bounds despawns are broken out separately, unlike
    // chaff_killed_total, which has always counted all three together.
    json spawned, killed, leaked, despawned;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        spawned[family_key(f)] = s.chaff_spawned_by_family[f];
        killed[family_key(f)] = s.chaff_killed_by_family[f];
        leaked[family_key(f)] = s.chaff_leaked_by_family[f];
        despawned[family_key(f)] = s.chaff_despawned_by_family[f];
    }
    j["chaff_spawned_by_family"] = std::move(spawned);
    j["chaff_killed_by_family"] = std::move(killed);
    j["chaff_leaked_by_family"] = std::move(leaked);
    j["chaff_despawned_by_family"] = std::move(despawned);
    return j;
}

/// Resolves an assertion's left-hand value from a snapshot.
bool read_metric(const sim::SimWorld& world, const std::string& metric, f64& out) {
    const sim::SimSnapshot s = world.snapshot();
    if (metric == "chaff_count")          { out = static_cast<f64>(s.chaff_count); return true; }
    if (metric == "named_count")          { out = static_cast<f64>(s.named_count); return true; }
    if (metric == "total_density")        { out = static_cast<f64>(s.total_density); return true; }
    if (metric == "objective_integrity")  { out = static_cast<f64>(s.objective_integrity); return true; }
    if (metric == "chaff_killed_total")   { out = static_cast<f64>(s.chaff_killed_total); return true; }
    if (metric == "chaff_leaked_total")   { out = static_cast<f64>(s.chaff_leaked_total); return true; }
    if (metric == "tick")                 { out = static_cast<f64>(s.tick); return true; }
    if (metric == "active_squads")        { out = static_cast<f64>(s.active_squads); return true; }
    if (metric == "state_hash")           { out = static_cast<f64>(world.state_hash()); return true; }

    // Per-family forms: "<counter>.<family>", e.g. "chaff_leaked.virus". Worth
    // having as assertions and not only as report keys -- "the bacteria got
    // through" is a regression a total-only metric cannot express, because a
    // level that kills more bacteria hides it.
    const usize dot = metric.find('.');
    if (dot != std::string::npos) {
        const std::string counter = metric.substr(0, dot);
        const std::string family = metric.substr(dot + 1);
        for (u32 f = 0; f < kFamilyCount; ++f) {
            if (family != family_key(f)) continue;
            if (counter == "chaff_spawned")   { out = static_cast<f64>(s.chaff_spawned_by_family[f]); return true; }
            if (counter == "chaff_killed")    { out = static_cast<f64>(s.chaff_killed_by_family[f]); return true; }
            if (counter == "chaff_leaked")    { out = static_cast<f64>(s.chaff_leaked_by_family[f]); return true; }
            if (counter == "chaff_despawned") { out = static_cast<f64>(s.chaff_despawned_by_family[f]); return true; }
            if (counter == "chaff_alive")     { out = static_cast<f64>(s.chaff_by_family[f]); return true; }
            break;
        }
    }
    return false;
}

bool compare(f64 lhs, const std::string& op, f64 rhs) {
    if (op == "==") return lhs == rhs;
    if (op == "!=") return lhs != rhs;
    if (op == "<")  return lhs < rhs;
    if (op == "<=") return lhs <= rhs;
    if (op == ">")  return lhs > rhs;
    if (op == ">=") return lhs >= rhs;
    return false;
}

} // namespace

std::unique_ptr<JobSystem> make_jobs(const Options& opt) {
    if (opt.threads == 1) return std::make_unique<JobSystem>(0u);
    if (opt.threads > 1) return std::make_unique<JobSystem>(static_cast<u32>(opt.threads - 1));
    return std::make_unique<JobSystem>();
}

// ---------------------------------------------------------------------------
// Scenario registry
// ---------------------------------------------------------------------------

const std::vector<BenchScenario>& bench_scenarios() {
    static const std::vector<BenchScenario> scenarios = {
        {"empty",    "No agents. Measures fixed per-tick overhead.",            0,     0, 0,  1024},
        {"chaff1k",  "1,000 chaff agents. Smoke-scale horde.",                  1000,  0, 0,  4096},
        {"chaff10k", "10,000 chaff agents. THE Wave 1 gate scenario (<4ms).",   10000, 0, 0,  16384},
        {"chaff10k_towers",
                     "10,000 chaff plus 16 persistent damage fields.",          10000, 0, 16, 16384},
        {"named200", "200 named ECS agents. The <2ms §8.6 budget.",             0,   200, 0,  1024},
        {"mixed",    "10,000 chaff + 200 named + 16 fields. Floodplain-like.",  10000, 200, 16, 16384},
    };
    return scenarios;
}

const BenchScenario* find_bench_scenario(const std::string& name) {
    for (const auto& s : bench_scenarios()) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::string resolve_config_dir(const Options& options) {
    return options.config_dir.empty() ? platform::asset_path("config") : options.config_dir;
}

int run_dump_config(const Options& options) {
    // Dumped from the values the game is running on right now, so the shipped
    // files start out byte-for-byte equivalent to the compiled-in tuning and
    // the migration cannot silently move a number.
    const game::GameConfig cfg = game::default_game_config();
    std::string err;
    if (!game::write_game_config(cfg, options.dump_config_dir, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& name : game::config_file_names()) {
        std::fprintf(stderr, "wrote %s/%s\n", options.dump_config_dir.c_str(), name.c_str());
    }
    return 0;
}

/// Loads assets/config (or --config) for a headless run. See Modes.h for why
/// the struct it fills is shared rather than private to this file.
HeadlessConfig load_headless_config(const Options& opt) {
    HeadlessConfig out;
    std::string err;
    if (!game::load_game_config(out.store, resolve_config_dir(opt), out.cfg, err)) {
        IMMUNE_LOG_ERROR("config load failed: %s", err.c_str());
        return out;
    }
    out.ok = true;
    out.hash = out.store.hash();
    return out;
}


int run_list_scenarios() {
    json j = json::array();
    for (const auto& s : bench_scenarios()) {
        j.push_back({{"name", s.name},
                     {"description", s.description},
                     {"chaff", s.chaff_count},
                     {"named", s.named_count},
                     {"damage_fields", s.damage_fields}});
    }
    std::printf("%s\n", j.dump(2).c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// --bench
// ---------------------------------------------------------------------------

int run_bench(const Options& opt) {
    const BenchScenario* scenario = find_bench_scenario(opt.scenario);
    if (!scenario) {
        IMMUNE_LOG_ERROR("unknown bench scenario '%s' (try --list-scenarios)",
                         opt.scenario.c_str());
        return 1;
    }

    auto jobs = make_jobs(opt);
    sim::SimWorld world;
    std::string error;
    if (!build_world(world, opt, scenario->max_chaff, jobs.get(), error)) {
        IMMUNE_LOG_ERROR("bench setup failed: %s", error.c_str());
        return 1;
    }
    populate_scenario(world, *scenario);

    Profiler profiler;
    profiler.reserve(static_cast<usize>(opt.ticks) + 1u);

    // Render submission is part of the §8.6 combined budget for chaff (flow
    // sample + separation + instanced render), so bench measures the real
    // submit path via a headless GL context rather than recording zero.
    // Failure here degrades to the old zero-recording behavior rather than
    // failing the whole bench run — a machine with no usable GL context
    // should still be able to measure the sim-side numbers.
    platform::Window render_window;
    render::Renderer renderer;
    render::Camera camera;
    bool have_renderer = false;
    if (platform::create_headless_gl(render_window, opt.width, opt.height)) {
        render::RendererDesc rd;
        rd.framebuffer_width = render_window.width();
        rd.framebuffer_height = render_window.height();
        rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
        if (renderer.init(rd)) {
            camera.set_viewport(render_window.width(), render_window.height());
            camera.set_bounds(world.desc().world_bounds);
            camera.set_center(world.desc().world_bounds.center());
            camera.set_view_height(world.desc().world_bounds.size().y);
            camera.clamp_to_bounds();
            have_renderer = true;
        } else {
            IMMUNE_LOG_WARN("bench: renderer init failed (%s); render_submit will read 0",
                            renderer.error().c_str());
        }
    } else {
        IMMUNE_LOG_WARN("bench: headless GL context failed (%s); render_submit will read 0",
                        render_window.error().c_str());
    }

    IMMUNE_LOG_INFO("bench '%s': %llu ticks, %llu chaff, %u threads, renderer %s",
                    scenario->name.c_str(),
                    static_cast<unsigned long long>(opt.ticks),
                    static_cast<unsigned long long>(world.chaff().count()),
                    jobs->thread_count(),
                    have_renderer ? "on" : "off");

    for (u64 i = 0; i < opt.ticks; ++i) {
        WallClock frame;
        world.tick(&profiler);
        if (have_renderer) {
            WallClock submit;
            renderer.begin_frame(camera, 0.0f);
            // No decor: --bench measures submission cost against the same
            // world every run, and the flow/lane textures are a --screenshot
            // and interactive-play concern.
            renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
            renderer.submit_chaff(world.chaff(), world.spatial());
            renderer.submit_entities(world.ecs());
            renderer.submit_fields(world.damage().rendered_fields().data(),
                                   world.damage().rendered_fields().size());
            renderer.end_frame();
            profiler.record(prof_key::kRenderSubmit, submit.elapsed_ms());
        } else {
            profiler.record(prof_key::kRenderSubmit, 0.0);
        }
        profiler.record(prof_key::kFrameTotal, frame.elapsed_ms());
    }

    const sim::SimSnapshot snap = world.snapshot();
    const std::string out = profiler.to_json(scenario->name, opt.ticks, opt.seed,
                                             snap.chaff_count, snap.named_count);
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// --sim-test
// ---------------------------------------------------------------------------
//
// SCRIPT SCHEMA (tests/scripts/*.json), version 1:
// {
//   "schema": 1,
//   "name": "chaff_despawns_at_goal",
//   "seed": 42,
//   "ticks": 600,
//   "max_chaff": 20000,
//   "level": "assets/levels/foo.json",        (optional)
//   "actions": [
//     {"tick": 0,  "type": "spawn_chaff", "family": "virus", "count": 500,
//      "pos": [16, 72], "radius": 4},
//     {"tick": 60, "type": "place_tower", "tower": "macrophage", "pos": [100, 72]},
//     {"tick": 90, "type": "cmd", "cmd": "spawn bacteria 300 at p0; tower all"}
//   ],
//   "assertions": [
//     {"tick": 600, "metric": "chaff_count", "op": "<", "value": 100},
//     {"tick": 600, "metric": "state_hash",  "op": "!=", "value": 0}
//   ]
// }
// Assertions run immediately after the named tick completes. Every assertion is
// evaluated (no early exit) so one run reports every failure.
//
// The "cmd" action runs a gym command (game/gym/GymCommands.h) against the same
// world, which is how a thing you found by hand in the console becomes a
// regression test without anyone porting it into a new action type first.
// Multiple commands may be separated by ';'. A failing command is logged and
// does not abort the run -- the assertions are what decide pass/fail.

int run_sim_test(const Options& opt) {
    const auto text = platform::read_text_file(opt.script_path);
    if (!text) {
        IMMUNE_LOG_ERROR("cannot read sim-test script '%s'", opt.script_path.c_str());
        return 1;
    }

    const HeadlessConfig tuning = load_headless_config(opt);
    if (!tuning.ok) return 1;

    json script;
    try {
        script = json::parse(*text);
    } catch (const std::exception& e) {
        IMMUNE_LOG_ERROR("sim-test script parse error: %s", e.what());
        return 1;
    }

    if (script.value("schema", 0) != 1) {
        IMMUNE_LOG_ERROR("sim-test script: unsupported or missing \"schema\" (expected 1)");
        return 1;
    }

    Options effective = opt;
    effective.seed = script.value("seed", opt.seed);
    if (script.contains("level")) effective.level = script.value("level", std::string{});
    const u64 total_ticks = script.value("ticks", u64{600});
    const usize max_chaff = script.value("max_chaff", usize{16384});

    auto jobs = make_jobs(effective);
    sim::SimWorld world;
    std::string error;
    if (!build_world(world, effective, max_chaff, jobs.get(), error)) {
        IMMUNE_LOG_ERROR("sim-test setup failed: %s", error.c_str());
        return 1;
    }

    const json actions = script.value("actions", json::array());
    const json assertions = script.value("assertions", json::array());

    json results = json::array();
    u32 passed = 0, failed = 0;

    game::TowerSystem towers;
    towers.register_systems(world);
    game::apply_tower_config(towers, tuning.cfg.towers);

    // The extra systems exist purely so a "cmd" action reaches the same surface
    // the in-game console does. A script that never issues one is unaffected:
    // none of them tick here, exactly as before.
    game::EnemyRoster roster;
    roster.load_defaults();
    game::WaveDirector waves;
    game::Economy economy;
    economy.configure(tuning.cfg.economy);
    game::ActiveAbilitySystem abilities;
    abilities.load_defaults();

    game::GymSpawnQueue gym_spawns;
    // Default OFF headlessly: a script asserting that integrity depletes must
    // still be able to observe that. `cmd: "invuln on"` turns it on explicitly.
    game::GymToggles gym_toggles;
    game::GymContext gym;
    gym.world = &world;
    gym.spawns = &gym_spawns;
    gym.toggles = &gym_toggles;
    gym.towers = &towers;
    gym.enemies = &roster;
    gym.waves = &waves;
    gym.economy = &economy;
    gym.abilities = &abilities;

    // Read/write the tuning from a script, so a balance regression can be
    // expressed as a sim-test rather than a hand-run experiment. Reload and
    // dump stay OFF headlessly: re-reading the files mid-run would change a
    // determinism input, and the report already pins the config hash.
    {
        // The store outlives the context: `tuning` is a local of this function
        // and the gym context never escapes it either.
        auto* registry = const_cast<config::Registry*>(&tuning.store.registry());
        game::bind_game_config(*registry, const_cast<game::GameConfig&>(tuning.cfg));
        gym.config_get = [registry](const std::string& path, std::string& out) {
            std::string err;
            if (registry->get(path, out, err)) return true;
            out = err;
            return false;
        };
        gym.config_set = [registry, &tuning, &towers, &roster, &economy, &abilities](
                             const std::string& path, const std::string& value, std::string& err) {
            if (!registry->set(path, value, err)) return false;
            // Push the change through to the live systems, or `config set`
            // would only edit a struct nothing reads.
            auto& cfg = const_cast<game::GameConfig&>(tuning.cfg);
            game::apply_tower_config(towers, cfg.towers);
            game::apply_enemy_config(roster, cfg.enemies);
            economy.configure(cfg.economy);
            game::apply_ability_config(abilities, cfg.abilities);
            return true;
        };
        gym.config_paths = [registry]() { return registry->field_paths(); };
    }

    auto run_actions_for_tick = [&](u64 tick) {
        for (const auto& a : actions) {
            if (a.value("tick", u64{0}) != tick) continue;
            const std::string type = a.value("type", std::string{});
            if (type == "spawn_chaff") {
                PathogenFamily fam = PathogenFamily::Virus;
                const std::string fname = a.value("family", std::string("virus"));
                for (u32 f = 0; f < kFamilyCount; ++f) {
                    // Family names mirror EnemyRoster::load_defaults.
                    static const char* names[kFamilyCount] = {"virus", "bacteria"};
                    if (fname == names[f]) fam = static_cast<PathogenFamily>(f);
                }
                const auto pos = a.value("pos", std::vector<f32>{0.0f, 0.0f});
                const f32 radius = a.value("radius", 3.0f);
                const u32 count = a.value("count", 0u);
                world.chaff_system().spawn_burst(
                    world.chaff(), fam,
                    Vec2{pos.size() > 0 ? pos[0] : 0.0f, pos.size() > 1 ? pos[1] : 0.0f},
                    radius, count, world.rng());
            } else if (type == "place_tower") {
                const std::string tname = a.value("tower", std::string{});
                TowerType ttype{};
                if (!game::parse_tower_type(tname, ttype)) {
                    IMMUNE_LOG_WARN("sim-test: unknown tower type '%s'", tname.c_str());
                } else {
                    const auto pos = a.value("pos", std::vector<f32>{0.0f, 0.0f});
                    const Vec2 world_pos{pos.size() > 0 ? pos[0] : 0.0f,
                                         pos.size() > 1 ? pos[1] : 0.0f};
                    const EntityId placed = towers.place(world, ttype, world_pos);
                    if (!placed.valid()) {
                        IMMUNE_LOG_WARN("sim-test: place_tower '%s' at (%.1f,%.1f) failed validation",
                                        tname.c_str(), world_pos.x, world_pos.y);
                    }
                }
            } else if (type == "cmd") {
                const std::string line = a.value("cmd", std::string{});
                const game::GymResult r = game::gym_execute_script(gym, line);
                if (!r.ok) {
                    IMMUNE_LOG_WARN("sim-test: cmd '%s' failed: %s", line.c_str(),
                                    r.message.c_str());
                } else if (!r.message.empty()) {
                    IMMUNE_LOG_INFO("sim-test: cmd '%s' -> %s", line.c_str(), r.message.c_str());
                }
            } else {
                IMMUNE_LOG_WARN("sim-test: unknown action type '%s'", type.c_str());
            }
        }
    };

    auto check_assertions_for_tick = [&](u64 tick) {
        for (const auto& as : assertions) {
            if (as.value("tick", u64{0}) != tick) continue;
            const std::string metric = as.value("metric", std::string{});
            const std::string op = as.value("op", std::string("=="));
            const f64 expected = as.value("value", 0.0);

            f64 actual = 0.0;
            json r;
            r["tick"] = tick;
            r["metric"] = metric;
            r["op"] = op;
            r["expected"] = expected;

            if (!read_metric(world, metric, actual)) {
                r["actual"] = nullptr;
                r["pass"] = false;
                r["error"] = "unknown metric";
                ++failed;
            } else {
                const bool ok = compare(actual, op, expected);
                r["actual"] = actual;
                r["pass"] = ok;
                ok ? ++passed : ++failed;
            }
            results.push_back(r);
        }
    };

    run_actions_for_tick(0);
    check_assertions_for_tick(0);
    for (u64 t = 1; t <= total_ticks; ++t) {
        run_actions_for_tick(t);
        gym_spawns.tick(world);
        world.tick(nullptr);
        gym_toggles.apply(world);
        check_assertions_for_tick(t);
    }

    json report;
    report["schema"] = 1;
    report["name"] = script.value("name", std::string("unnamed"));
    report["script"] = opt.script_path;
    report["seed"] = effective.seed;
    report["ticks"] = total_ticks;
    report["passed"] = passed;
    report["failed"] = failed;
    report["assertions"] = results;
    report["final_state"] = snapshot_to_json(world.snapshot());
    report["state_hash"] = world.state_hash();
    // Config is a determinism input now: the same seed and script only
    // reproduce a run if the tuning matches too.
    report["config_hash"] = tuning.hash;
    report["config_dir"] = resolve_config_dir(opt);
    report["result"] = failed == 0 ? "PASS" : "FAIL";

    std::printf("%s\n", report.dump(2).c_str());
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --screenshot
// ---------------------------------------------------------------------------

int run_screenshot(const Options& opt) {
    auto jobs = make_jobs(opt);
    sim::SimWorld world;
    std::string error;
    game::LaneOwnershipMap lanes;
    if (!build_world(world, opt, 16384, jobs.get(), error, &lanes)) {
        IMMUNE_LOG_ERROR("screenshot setup failed: %s", error.c_str());
        return 1;
    }

    // Optional: --scenario populates chaff/named agents from a bench_scenarios()
    // entry before capture, so a screenshot can show a live horde rather than
    // bare tissue. Independent of --level, which only picks the vessel geometry.
    if (!opt.scenario.empty()) {
        const BenchScenario* scenario = find_bench_scenario(opt.scenario);
        if (scenario == nullptr) {
            IMMUNE_LOG_ERROR("unknown --scenario '%s' (see --list-scenarios)", opt.scenario.c_str());
            return 1;
        }
        populate_scenario(world, *scenario);
    }

    // Place one of every tower type so a screenshot can actually show combat.
    // Without this, run_screenshot rendered a horde walking through an empty
    // level: no tower ever fired, so no projectile existed and no combat event
    // was ever raised, so the particle layer had nothing to draw. Wave 6C hit
    // exactly this and correctly reported it rather than editing this
    // orchestrator-owned file.
    //
    // Placement walks along the vessel's mid-line and takes the first spot
    // validate() accepts for each type, nudging across a few offsets. Towers
    // that find no legal spot are simply skipped -- a screenshot is a
    // diagnostic, not a gameplay guarantee.
    game::TowerSystem towers;
    towers.register_systems(world);
    {
        const HeadlessConfig tuning = load_headless_config(opt);
        if (!tuning.ok) return 1;
        game::apply_tower_config(towers, tuning.cfg.towers);
    }
    if (opt.place_towers) {
        TowerType only = TowerType::Count;
        if (!opt.tower_filter.empty()) {
            if (!game::parse_tower_type(opt.tower_filter, only)) {
                IMMUNE_LOG_ERROR("unknown --tower '%s'", opt.tower_filter.c_str());
                return 1;
            }
        }
        const Rect b = world.desc().world_bounds;
        const f32 mid_y = b.center().y;
        u32 placed = 0;
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const auto type = static_cast<TowerType>(t);
            if (only != TowerType::Count && type != only) continue;
            const f32 frac = (static_cast<f32>(t) + 1.0f) / (kTowerTypeCount + 1.0f);
            const f32 x = b.min.x + b.size().x * frac;
            for (const f32 dy : {0.0f, 4.0f, -4.0f, 8.0f, -8.0f, 12.0f, -12.0f}) {
                const Vec2 p{x, mid_y + dy};
                if (towers.validate(world, type, p, 1'000'000u).valid() &&
                    towers.place(world, type, p).valid()) {
                    ++placed;
                    break;
                }
            }
        }
        IMMUNE_LOG_INFO("screenshot: placed %u/%u towers", placed, kTowerTypeCount);
    }

    // Gym commands, before any ticking: --exec is how a console session becomes
    // a reproducible capture. The spawn queue is drained alongside the sim
    // below, so a command that asks for more agents than one burst can hold
    // still delivers all of them.
    game::GymSpawnQueue gym_spawns;
    game::GymToggles gym_toggles;
    if (!opt.exec.empty()) {
        game::EnemyRoster exec_roster;
        exec_roster.load_defaults();
        game::Economy exec_economy;
        exec_economy.configure(load_headless_config(opt).cfg.economy);
        game::ActiveAbilitySystem exec_abilities;
        exec_abilities.load_defaults();
        game::WaveDirector exec_waves;

        game::GymContext gym;
        gym.world = &world;
        gym.towers = &towers;
        gym.enemies = &exec_roster;
        gym.waves = &exec_waves;
        gym.economy = &exec_economy;
        gym.abilities = &exec_abilities;
        gym.spawns = &gym_spawns;
        gym.toggles = &gym_toggles;
        const game::GymResult r = game::gym_execute_script(gym, opt.exec);
        IMMUNE_LOG_INFO("--exec: %s", r.message.c_str());
        if (!r.ok) {
            IMMUNE_LOG_ERROR("--exec failed; capturing anyway so the failure is visible");
        }
    }

    // Advance the deterministic sim to the requested tick before rendering,
    // draining combat events and stepping particles ONCE PER TICK as we go --
    // exactly the cadence App::render_frame() uses at 60 FPS. Draining once at
    // the very end and only stepping particles a few catch-up frames (the
    // first version of this code did that) crushes an entire run's worth of
    // muzzle flashes into one overlapping blob a few pixels wide instead of
    // the spread stream continuous play actually produces -- it looked like a
    // tower's attack was barely there when the tower was firing correctly the
    // whole time. A screenshot is only useful as a diagnostic if it shows what
    // real play looks like.
    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, opt.seed ^ 0xA5A5'5A5AULL);
    for (u64 i = 0; i < opt.ticks; ++i) {
        gym_spawns.tick(world);
        world.tick(nullptr);
        gym_toggles.apply(world);
        const auto& evts = world.combat_events().events();
        particles.emit_for_events(evts.data(), evts.size());
        world.combat_events().clear();
        particles.update(kFixedDt, nullptr);
    }

    platform::Window window;
    if (!platform::create_headless_gl(window, opt.width, opt.height)) {
        IMMUNE_LOG_ERROR("headless GL context creation failed: %s", window.error().c_str());
        return 1;
    }

    render::RendererDesc rd;
    rd.framebuffer_width = window.width();
    rd.framebuffer_height = window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    if (!renderer.init(rd)) {
        IMMUNE_LOG_ERROR("renderer init failed: %s", renderer.error().c_str());
        return 1;
    }

    render::Camera camera;
    camera.set_viewport(window.width(), window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(opt.has_focus ? opt.focus : world.desc().world_bounds.center());
    camera.set_view_height(opt.view_height > 0.0f ? opt.view_height
                                                  : world.desc().world_bounds.size().y);
    camera.clamp_to_bounds();

    std::vector<vfx::ParticleInstance> pinst;
    pinst.reserve(vfx::ParticleSystem::kDefaultCapacity);

    renderer.begin_frame(camera, 0.0f);
    const render::TissueDecor decor = tissue_decor(world, lanes);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f, &decor);
    renderer.submit_chaff(world.chaff(), world.spatial());
    renderer.submit_entities(world.ecs());
    renderer.submit_fields(world.damage().rendered_fields().data(),
                           world.damage().rendered_fields().size());
    renderer.submit_projectiles(world.projectiles());
    renderer.submit_swarmers(world.swarmers());
    renderer.submit_fluid(world.fluid(), world.fluid_system().draw_radius());
    particles.build_instances(vfx::BlendMode::Additive, pinst);
    renderer.submit_particles(pinst.data(), pinst.size(), vfx::BlendMode::Additive);
    particles.build_instances(vfx::BlendMode::AlphaBlend, pinst);
    renderer.submit_particles(pinst.data(), pinst.size(), vfx::BlendMode::AlphaBlend);
    renderer.end_frame();
    // Field count included because a missing AoE is otherwise indistinguishable
    // from an AoE that drew at zero alpha, and the two have very different fixes.
    IMMUNE_LOG_INFO("screenshot: %zu live rounds, %zu live swarmers, %zu fluid particles, "
                    "%zu live particles, %zu damage fields",
                    world.projectiles().count(), world.swarmers().count(), world.fluid().count(),
                    particles.live_count(), world.damage().rendered_fields().size());

    if (!render::capture_framebuffer_png(opt.out_path, window.width(), window.height())) {
        IMMUNE_LOG_ERROR("PNG write failed: %s", opt.out_path.c_str());
        return 1;
    }

    json meta;
    meta["schema"] = 1;
    meta["out"] = opt.out_path;
    meta["width"] = window.width();
    meta["height"] = window.height();
    meta["tick"] = opt.ticks;
    meta["seed"] = opt.seed;
    meta["level"] = opt.level.empty() ? std::string("default_test") : opt.level;
    meta["state"] = snapshot_to_json(world.snapshot());
    meta["state_hash"] = world.state_hash();
    std::printf("%s\n", meta.dump(2).c_str());
    std::fflush(stdout);
    return 0;
}

} // namespace immune::app
