#include "app/Modes.h"

#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include "core/Rng.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>

namespace immune::app {
namespace {

using json = nlohmann::json;

/// Builds the sim for a headless run: level geometry (from --level or the
/// built-in test level) plus deterministic tuning.
bool build_world(sim::SimWorld& world, const Options& opt, usize max_chaff,
                 JobSystem* jobs, std::string& error) {
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
    world.init(desc, jobs);

    const auto res = loader.instantiate(level, world);
    if (!res.ok) {
        error = res.error;
        return false;
    }
    return true;
}

/// Populates a world for a bench scenario at t=0.
void populate_scenario(sim::SimWorld& world, const BenchScenario& s) {
    if (s.chaff_count == 0) return;
    const Rect b = world.desc().world_bounds;
    Rng& rng = world.rng();
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

json snapshot_to_json(const sim::SimSnapshot& s) {
    json j;
    j["tick"] = s.tick;
    j["chaff_count"] = s.chaff_count;
    j["named_count"] = s.named_count;
    j["total_density"] = s.total_density;
    j["objective_integrity"] = s.objective_integrity;
    j["chaff_killed_total"] = s.chaff_killed_total;
    j["chaff_leaked_total"] = s.chaff_leaked_total;
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
    if (metric == "state_hash")           { out = static_cast<f64>(world.state_hash()); return true; }
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

std::unique_ptr<JobSystem> make_jobs(const Options& opt) {
    if (opt.threads == 1) return std::make_unique<JobSystem>(0u);
    if (opt.threads > 1) return std::make_unique<JobSystem>(static_cast<u32>(opt.threads - 1));
    return std::make_unique<JobSystem>();
}

} // namespace

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

    IMMUNE_LOG_INFO("bench '%s': %llu ticks, %llu chaff, %u threads",
                    scenario->name.c_str(),
                    static_cast<unsigned long long>(opt.ticks),
                    static_cast<unsigned long long>(world.chaff().count()),
                    jobs->thread_count());

    for (u64 i = 0; i < opt.ticks; ++i) {
        WallClock frame;
        world.tick(&profiler);
        // Render submission is measured even headlessly: the CPU-side cost of
        // walking the SoA and building instance ranges is a sim-side cost and
        // belongs in the budget. With no GL context it records ~0 today, and
        // Wave 1C replaces this with the real submit path.
        profiler.record(prof_key::kRenderSubmit, 0.0);
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
//     {"tick": 60, "type": "place_tower", "tower": "macrophage", "pos": [100, 72]}
//   ],
//   "assertions": [
//     {"tick": 600, "metric": "chaff_count", "op": "<", "value": 100},
//     {"tick": 600, "metric": "state_hash",  "op": "!=", "value": 0}
//   ]
// }
// Assertions run immediately after the named tick completes. Every assertion is
// evaluated (no early exit) so one run reports every failure.

int run_sim_test(const Options& opt) {
    const auto text = platform::read_text_file(opt.script_path);
    if (!text) {
        IMMUNE_LOG_ERROR("cannot read sim-test script '%s'", opt.script_path.c_str());
        return 1;
    }

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

    auto run_actions_for_tick = [&](u64 tick) {
        for (const auto& a : actions) {
            if (a.value("tick", u64{0}) != tick) continue;
            const std::string type = a.value("type", std::string{});
            if (type == "spawn_chaff") {
                PathogenFamily fam = PathogenFamily::Virus;
                const std::string fname = a.value("family", std::string("virus"));
                for (u32 f = 0; f < kFamilyCount; ++f) {
                    // Family names mirror EnemyRoster::load_defaults.
                    static const char* names[kFamilyCount] = {
                        "virus", "bacteria", "fungal_spore", "parasite", "cancer_cell", "allergen"};
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
                // Wave 2B wires this through TowerSystem::place.
                IMMUNE_LOG_WARN("sim-test action 'place_tower' is not implemented until Wave 2B");
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
        world.tick(nullptr);
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
    if (!build_world(world, opt, 16384, jobs.get(), error)) {
        IMMUNE_LOG_ERROR("screenshot setup failed: %s", error.c_str());
        return 1;
    }

    // Advance the deterministic sim to the requested tick before rendering.
    world.run_ticks(opt.ticks, nullptr);

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
    camera.set_center(world.desc().world_bounds.center());
    camera.set_view_height(world.desc().world_bounds.size().y);
    camera.clamp_to_bounds();

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
    renderer.submit_chaff(world.chaff(), world.spatial());
    renderer.submit_entities(world.ecs());
    renderer.submit_fields(world.damage().fields().data(), world.damage().fields().size());
    renderer.end_frame();

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
