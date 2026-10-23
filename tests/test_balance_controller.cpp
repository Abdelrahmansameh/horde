#include "BalanceTestIsolation.h"
#include "core/Math.h"
#include "game/balance/BalanceController.h"
#include "game/balance/BalanceRun.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"
#include "sim/flowfield/RuntimeBlock.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace immune;
using namespace immune::game;
using namespace immune::game::balance;

namespace {

struct ControllerFixture {
    LevelDef level;
    sim::SimWorld world;
    TowerSystem towers;
    Economy economy;
    WaveDirector waves;
    ActiveAbilitySystem abilities;

    void init(u32 starting_atp = 300) {
        const auto parsed = LevelLoader{}.load_string(R"JSON({
          "schema":2,"name":"balance_controller_fixture","region":"capillary",
          "world":{"min":[0,0],"max":[180,100],"cell_size":1},
          "vessels":[{"id":"main","points":[{"p":[4,50],"w":42},{"p":[176,50],"w":42}]}],
          "spawn_points":[{"id":"p0","pos":[4,50],"radius":3,"lane_id":"main"}],
          "objectives":[{"id":"organ","pos":[176,50],"half_extents":[3,18]}],
          "placement_zones":[{"min":[12,30],"max":[165,70]}],
          "squad_paths":[{"id":"route","lane_id":"main","points":[[4,50],[176,50]],"half_width":6}],
          "waves":[{"name":"w1","prep_time":1,"spawns":[
            {"family":"virus","count":30,"duration":1,"spawn_point_id":"p0","squad_paths":["route"]}
          ]}]
        })JSON", level);
        INFO(parsed.error);
        REQUIRE(parsed.ok);
        sim::SimDesc desc;
        desc.seed = 123;
        desc.max_chaff = 512;
        desc.max_swarmers = 512;
        desc.max_fluid_particles = 128;
        desc.world_bounds = level.world_bounds;
        desc.sim_bounds = level_sim_bounds(level);
        world.init(desc, nullptr);
        REQUIRE(LevelLoader{}.instantiate(level, world).ok);
        towers.register_systems(world);
        EconomyConfig config;
        config.starting_atp = starting_atp;
        economy.configure(config);
        waves.set_waves(level.waves);
        waves.set_auto_start(false);
        waves.start(world);
        abilities.load_defaults();
        abilities.set_unlocked(0);
        abilities.register_systems(world);
    }

    ControllerView view() const { return {world, level, towers, economy, waves, abilities}; }

    void spawn(Vec2 position, u32 quantity, bool hidden = false) {
        for (u32 i = 0; i < quantity; ++i) {
            sim::ChaffSpawnParams spawn;
            spawn.position = position + Vec2{static_cast<f32>(i % 5) * 0.3f,
                                            static_cast<f32>(i / 5) * 0.3f};
            if (hidden) spawn.flags = sim::chaff_flags::kHidden;
            REQUIRE(world.chaff().spawn(spawn).valid());
        }
    }
};

const Action* deploy_action(const std::vector<Action>& actions) {
    const auto found = std::find_if(actions.begin(), actions.end(), [](const Action& action) {
        return action.kind == ActionKind::Deploy;
    });
    return found != actions.end() ? &*found : nullptr;
}

f32 path_distance(Vec2 position, const config::Json& path) {
    f32 nearest = std::numeric_limits<f32>::infinity();
    const auto& points = path.at("points");
    for (usize i = 1; i < points.size(); ++i) {
        const Vec2 a{points[i - 1][0].get<f32>(), points[i - 1][1].get<f32>()};
        const Vec2 b{points[i][0].get<f32>(), points[i][1].get<f32>()};
        const Vec2 d = b - a, offset = position - a;
        const f32 length_sq = d.x * d.x + d.y * d.y;
        const f32 t = length_sq > 0 ? std::clamp((offset.x * d.x + offset.y * d.y) / length_sq, 0.0f, 1.0f) : 0;
        const Vec2 delta = position - (a + d * t);
        nearest = std::min(nearest, std::sqrt(delta.x * delta.x + delta.y * delta.y));
    }
    return nearest;
}

f32 path_remaining(Vec2 position, const config::Json& path) {
    f32 nearest = std::numeric_limits<f32>::infinity(), total = 0.0f, matched = 0.0f;
    const auto& points = path.at("points");
    for (usize i = 1; i < points.size(); ++i) {
        const Vec2 a{points[i - 1][0].get<f32>(), points[i - 1][1].get<f32>()};
        const Vec2 b{points[i][0].get<f32>(), points[i][1].get<f32>()};
        const Vec2 direction = b - a, offset = position - a;
        const f32 length_sq = math::length_sq(direction), length = std::sqrt(length_sq);
        const f32 t = length_sq > 0.0f ? std::clamp(
            (offset.x * direction.x + offset.y * direction.y) / length_sq, 0.0f, 1.0f) : 0.0f;
        const f32 distance = math::length_sq(position - (a + direction * t));
        if (distance < nearest) { nearest = distance; matched = total + length * t; }
        total += length;
    }
    return total - matched;
}

// Independent, fine-step observation of the public navigation field. Unlike
// the controller's cached/simplified heuristic this keeps its full polyline,
// so physical coverage tests need not mistake soft squad waypoints for traffic.
config::Json navigation_path(const sim::SimWorld& world, Vec2 position,
                             config::Json* diagnostic = nullptr, f32 step = 1.0f) {
    config::Json points = config::Json::array();
    std::string reason = "iteration_limit";
    config::Json first_uphill = nullptr;
    for (u32 i = 0; i < 4096; ++i) {
        points.push_back({position.x, position.y});
        const f32 cost = world.flow().sample_cost(position);
        if (std::isfinite(cost) && cost <= world.flow().cell_size() * 0.5f) { reason = "goal"; break; }
        Vec2 direction = world.flow().sample(position);
        if (math::length_sq(direction) <= math::kEpsilon) direction = world.sdf().gradient(position);
        direction = math::normalize_safe(direction);
        if (math::length_sq(direction) <= math::kEpsilon) { reason = "no_direction"; break; }
        Vec2 next = position + direction * step;
        const f32 margin = std::max(0.25f, world.flow().cell_size() * 0.5f);
        for (u32 correction = 0; correction < 4; ++correction) {
            const f32 clearance = world.sdf().sample(next);
            if (clearance >= margin) break;
            const Vec2 normal = math::normalize_safe(world.sdf().gradient(next));
            if (math::length_sq(normal) <= math::kEpsilon) break;
            next += normal * (margin - clearance);
        }
        sim::contain_to_walkable(world.tissue(), next, position);
        if (!world.desc().sim_bounds.contains(next) || world.sdf().sample(next) <= 0.0f ||
            math::length_sq(next - position) < 0.01f) { reason = "blocked"; break; }
        const f32 next_cost = world.flow().sample_cost(next);
        if (first_uphill.is_null() && std::isfinite(cost) &&
            (!std::isfinite(next_cost) || next_cost > cost + 0.01f))
            first_uphill = {{"position", {position.x, position.y}}, {"cost", cost},
                {"next_cost", std::isfinite(next_cost) ? config::Json(next_cost) : config::Json(nullptr)}};
        position = next;
    }
    if (diagnostic != nullptr) *diagnostic = {{"termination", reason}, {"samples", points.size()},
        {"endpoint", {position.x, position.y}}, {"first_uphill", first_uphill}};
    return {{"points", points}, {"complete", reason == "goal"}};
}

std::vector<config::Json> navigation_corridors(const sim::SimWorld& world, const SpawnPoint& spawn) {
    auto centre = navigation_path(world, spawn.position);
    REQUIRE(centre["complete"] == true);
    const Vec2 direction = math::normalize_safe(world.flow().sample(spawn.position));
    const Vec2 lateral{-direction.y, direction.x};
    std::vector<config::Json> result;
    for (f32 offset : {-0.5f, 0.5f}) {
        auto side = navigation_path(world, spawn.position + lateral * (spawn.radius * offset));
        if (side["complete"] == true) result.push_back(std::move(side));
    }
    if (result.size() != 2) return {std::move(centre)};
    return result;
}

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller proposes legal paid actions without changing gameplay",
          "[balance][controller][deployment]") {
    ControllerFixture fixture;
    fixture.init();
    fixture.towers.set_unlocked_towers(1u << static_cast<u32>(TowerType::Neutrophil));
    fixture.towers.set_allowed_towers(1u << static_cast<u32>(TowerType::Neutrophil));
    BalanceController controller;
    const auto hash = fixture.world.state_hash();
    const auto before = fixture.economy.snapshot();
    const auto actions = controller.decide(fixture.view());
    const Action* purchase = deploy_action(actions);
    REQUIRE(purchase != nullptr);
    CHECK(purchase->tower == TowerType::Neutrophil);
    CHECK(purchase->quantity == 1);
    CHECK(purchase->tick == fixture.world.tick_index());
    CHECK(fixture.towers.validate_deploy(fixture.world, purchase->tower,
                                        purchase->position, before.atp).valid());
    CHECK(fixture.world.state_hash() == hash);
    CHECK(fixture.world.swarmers().count() == 0);
    CHECK(fixture.economy.atp() == before.atp);
    CHECK(fixture.economy.snapshot().total_spent == before.total_spent);
    CHECK(fixture.waves.status().phase == WavePhase::Prep);
    CHECK(controller.decide(fixture.view()).empty());

    const u32 cost = fixture.towers.stats(purchase->tower).build_cost;
    REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                         purchase->tower, purchase->position, purchase->quantity) == 1);
    CHECK(fixture.economy.atp() == before.atp - cost);
    CHECK(fixture.economy.snapshot().total_spent == before.total_spent + cost);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller specialist weights enforce ablation and ATP reserve",
          "[balance][controller][economy]") {
    ControllerFixture fixture;
    fixture.init();
    ControllerParams params;
    params.policy = Policy::Specialist;
    params.weights.fill(0.0f);
    SECTION("zero composition never falls back to another unit") {
        BalanceController controller(params);
        CHECK(deploy_action(controller.decide(fixture.view())) == nullptr);
    }
    SECTION("single type is selected through actual affordability") {
        params.weights[static_cast<u32>(TowerType::CytotoxicT)] = 1.0f;
        BalanceController controller(params);
        const auto actions = controller.decide(fixture.view());
        const Action* action = deploy_action(actions);
        REQUIRE(action != nullptr);
        CHECK(action->tower == TowerType::CytotoxicT);
    }
    SECTION("reserve cannot be overspent") {
        params.weights[static_cast<u32>(TowerType::Neutrophil)] = 1.0f;
        params.reserve_atp = fixture.economy.atp();
        BalanceController controller(params);
        CHECK(deploy_action(controller.decide(fixture.view())) == nullptr);
    }
    SECTION("locked specialist never substitutes an unlocked type") {
        params.weights[static_cast<u32>(TowerType::CytotoxicT)] = 1.0f;
        fixture.towers.set_unlocked_towers(1u << static_cast<u32>(TowerType::Neutrophil));
        BalanceController controller(params);
        CHECK(deploy_action(controller.decide(fixture.view())) == nullptr);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller observes visible pressure and ignores hidden enemies",
          "[balance][controller][visibility]") {
    ControllerFixture plain, concealed, threatened;
    plain.init();
    concealed.init();
    threatened.init();
    concealed.spawn(Vec2{145.0f, 50.0f}, 60, true);
    threatened.spawn(Vec2{145.0f, 50.0f}, 60);
    ControllerParams params;
    params.policy = Policy::Specialist;
    params.weights = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    BalanceController first(params), second(params), third(params);
    const auto plain_actions = first.decide(plain.view());
    const auto concealed_actions = second.decide(concealed.view());
    const auto threatened_actions = third.decide(threatened.view());
    const Action* a = deploy_action(plain_actions);
    const Action* b = deploy_action(concealed_actions);
    const Action* c = deploy_action(threatened_actions);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->position == b->position);
    CHECK(c->position.x > a->position.x + 20.0f);
    CHECK(threatened.towers.validate_deploy(threatened.world, c->tower,
                                           c->position, threatened.economy.atp()).valid());
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller replay decisions use independent deterministic placement variation",
          "[balance][controller][determinism]") {
    ControllerFixture fixture;
    fixture.init();
    ControllerParams params;
    params.policy = Policy::Specialist;
    params.weights = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    params.placement_seed = 481;
    BalanceController first(params), second(params);
    const auto a = first.decide(fixture.view());
    const auto b = second.decide(fixture.view());
    REQUIRE(a.size() == b.size());
    REQUIRE(deploy_action(a) != nullptr);
    REQUIRE(deploy_action(b) != nullptr);
    CHECK(deploy_action(a)->position == deploy_action(b)->position);
    first.reset();
    const auto repeated = first.decide(fixture.view());
    REQUIRE(deploy_action(repeated) != nullptr);
    CHECK(deploy_action(a)->position == deploy_action(repeated)->position);
    bool changed = false;
    for (u64 seed = 482; seed < 488; ++seed) {
        params.placement_seed = seed;
        BalanceController variant(params);
        const auto actions = variant.decide(fixture.view());
        REQUIRE(deploy_action(actions) != nullptr);
        changed = changed || deploy_action(actions)->position != deploy_action(a)->position;
    }
    CHECK(changed);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller establishes mutually supporting mobile cells during preparation",
          "[balance][controller][formation]") {
    ControllerFixture fixture;
    fixture.init();
    fixture.towers.set_unlocked_towers(1u << static_cast<u32>(TowerType::Neutrophil));
    ControllerParams params;
    params.reaction_ticks = 1;
    BalanceController controller(params);
    std::vector<Vec2> purchases;
    for (u32 i = 0; i < 20; ++i) {
        const auto actions = controller.decide(fixture.view());
        const Action* action = deploy_action(actions);
        REQUIRE(action != nullptr);
        purchases.push_back(action->position);
        REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                             action->tower, action->position, 1) == 1);
        fixture.world.tick();
    }
    for (const Vec2 p : purchases) {
        u32 supporting = 0;
        for (const Vec2 other : purchases) {
            const Vec2 delta = p - other;
            if (delta.x * delta.x + delta.y * delta.y <= 20.0f * 20.0f) ++supporting;
        }
        // More than half the opening defense can engage a common target.
        CHECK(supporting >= 12);
    }
    CHECK(fixture.economy.snapshot().total_spent ==
          purchases.size() * fixture.towers.stats(TowerType::Neutrophil).build_cost);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "fresh controller clears the real opening wave with supporting paid cells",
          "[balance][controller][campaign]") {
    config::ConfigStore store;
    GameConfig config;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), config, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_01_first_bend.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    level["waves"].erase(level["waves"].begin() + 1, level["waves"].end());
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 101},
        {"max_ticks", 6000}, {"sample_ticks", 300},
        {"progression", {{"memory_cells", 0}, {"antibodies", 0}}},
        {"controller", {{"policy", "competent"}, {"use_abilities", false}}}};
    const auto report = execute(spec, config);
    INFO(report.at("diagnostics").dump());
    INFO(report.at("summary").dump());
    CHECK(report.at("summary").at("valid") == true);
    CHECK(report.at("summary").at("status") == "cleared");
    CHECK(report.at("summary").at("abilities_used") == 0);
    CHECK(report.at("summary").at("actions_rejected") == 0);
    CHECK(report.at("summary").at("objective_integrity").get<f64>() > 0.0);
    CHECK(report.at("economy").at("opening_atp") == 300);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller prepares a supporting cohort for each authored entrance",
          "[balance][controller][formation][routes]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_03_two_chambers.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    SECTION("one preview entry per approach") {}
    SECTION("repeated entries on each approach share their coverage") {
        // Several enemy families can use the same path in a wave. They add
        // expected pressure, rather than defining empty extra defense lanes.
        const auto upper = level["waves"][0]["spawns"][0];
        const auto lower = level["waves"][0]["spawns"][1];
        level["waves"][0]["spawns"].push_back(upper);
        level["waves"][0]["spawns"].push_back(lower);
    }
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 3301},
        {"max_ticks", 600}, {"sample_ticks", 300},
        {"progression", {{"memory_cells", 0}, {"antibodies", 0}}},
        {"controller", {{"policy", "tactical"}, {"use_abilities", false}, {"prep_ticks", 1000}}}};
    std::array<std::vector<config::Json>, 2> corridors;
    const auto report = execute(spec, base, [&](const CaptureView& view) {
        REQUIRE(view.level.spawn_points.size() == 2);
        for (usize i = 0; i < 2; ++i) corridors[i] = navigation_corridors(view.world, view.level.spawn_points[i]);
    });
    INFO(report.at("diagnostics").dump());
    f32 upper = 0.0f, lower = 0.0f;
    for (const auto& action : report.at("actions")) {
        if (action.at("kind") != "deploy") continue;
        REQUIRE(action.at("accepted") == true);
        const Vec2 position{action["position"][0].get<f32>(), action["position"][1].get<f32>()};
        std::array<f32, 2> membership{};
        for (usize approach = 0; approach < 2; ++approach) for (const auto& path : corridors[approach])
            if (path_distance(position, path) <= 20.0f) membership[approach] += 1.0f / static_cast<f32>(corridors[approach].size());
        const f32 total = membership[0] + membership[1];
        REQUIRE(total > 0.0f);
        // A cell at a genuine shared junction helps both entrances, with its
        // physical contribution split rather than counted twice.
        upper += membership[0] / total;
        lower += membership[1] / total;
    }
    CHECK(upper >= 5);
    CHECK(lower >= 5);
    CHECK(std::max(upper, lower) - std::min(upper, lower) <= 3);
    CHECK(report.at("summary").at("actions_rejected") == 0);
    CHECK(report.at("wave_status").at("phase") == static_cast<u32>(WavePhase::Prep));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller intercepts public fork alternatives before the current wave switches branches",
          "[balance][controller][formation][fork]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_04_twin_channels.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    const auto dorsal = level["squad_paths"][2], ventral = level["squad_paths"][3];
    bool weak_support = false;
    SECTION("only the dorsal branch appears in the current wave") {}
    SECTION("path order and duplicated family entries do not hide the other branch") {
        std::reverse(level["squad_paths"].begin(), level["squad_paths"].end());
        const auto upper = level["waves"][0]["spawns"][0], lower = level["waves"][0]["spawns"][1];
        level["waves"][0]["spawns"].push_back(upper);
        level["waves"][0]["spawns"].push_back(lower);
    }
    SECTION("a short-search support unlock cannot veto combat anchors") {
        weak_support = true;
        base.towers.mechanics[static_cast<u32>(TowerType::Fibroblast)].swarm.search_radius = 0.1f;
    }
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 7133},
        {"max_ticks", 30}, {"sample_ticks", 30}, {"progression", {{"profile", "all_unlocked"}}},
        {"controller", {{"policy", "specialist"}, {"weights", {{"neutrophil", 1}, {"macrophage", 1},
                                                                {"fibroblast", weak_support ? 0.00001 : 0.0}}},
                        {"use_abilities", false}, {"reaction_ticks", 1}, {"prep_ticks", 1000}}}};
    const auto report = execute(spec, base);
    INFO(report.at("diagnostics").dump());
    u32 lower = 0;
    for (const auto& action : report.at("actions")) {
        if (action.at("kind") != "deploy") continue;
        REQUIRE(action.at("accepted") == true);
        const Vec2 position{action["position"][0].get<f32>(), action["position"][1].get<f32>()};
        if (position.y >= 140.0f) continue;
        ++lower;
        CHECK(path_distance(position, dorsal) <= 15.0f);
        CHECK(path_distance(position, ventral) <= 15.0f);
        CHECK(position.x < 70.0f);
    }
    CHECK(lower >= 5);
    CHECK(report.at("summary").at("actions_rejected") == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "a large wrong-branch cohort cannot attract the next purchase away from shared interception",
          "[balance][controller][formation][fork]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_04_twin_channels.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    level["waves"][0]["spawns"] = config::Json::array({level["waves"][0]["spawns"][1]});
    config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 7133},
        {"max_ticks", 1}, {"opening_atp", 2000},
        {"actions", config::Json::array()}};
    for (u32 i = 0; i < 8; ++i) spec["actions"].push_back({{"tick", 0}, {"kind", "deploy"},
        {"tower", "neutrophil"}, {"position", {133, 96}}, {"quantity", 10}});
    std::vector<Action> proposed;
    bool captured = false;
    usize friendly_count = 0;
    const auto report = execute(spec, base, [&](const CaptureView& view) {
        captured = true;
        friendly_count = view.world.swarmers().count();
        CHECK(path_distance(Vec2{133.0f, 96.0f}, level["squad_paths"][3]) > 15.0f);
        ControllerParams params;
        params.policy = Policy::Specialist;
        params.weights = {1, 0, 0, 0, 0};
        BalanceController controller(params);
        proposed = controller.decide({view.world, view.level, view.towers, view.economy, view.waves, view.abilities});
    });
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(captured);
    REQUIRE(friendly_count == 80);
    const Action* action = deploy_action(proposed);
    REQUIRE(action != nullptr);
    CHECK(path_distance(action->position, level["squad_paths"][2]) <= 15.0f);
    CHECK(path_distance(action->position, level["squad_paths"][3]) <= 15.0f);
    CHECK(action->position.x < 70.0f);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "fork planning preserves pure mobile and builder specialist placements",
          "[balance][controller][fork][ablation]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_04_twin_channels.json"));
    REQUIRE(text.has_value());
    for (const char* type : {"cytotoxic_t", "goblet_cell", "fibroblast"}) {
        INFO(type);
        const config::Json spec = {{"schema", 1}, {"level_document", config::Json::parse(*text)}, {"seed", 7133},
            {"max_ticks", 1}, {"progression", {{"profile", "all_unlocked"}}},
            {"controller", {{"policy", "specialist"}, {"weights", {{type, 1}}}, {"use_abilities", false}}}};
        const auto report = execute(spec, base);
        INFO(report.at("diagnostics").dump());
        REQUIRE(!report.at("actions").empty());
        CHECK(report.at("actions")[0].at("kind") == "deploy");
        CHECK(report.at("actions")[0].at("tower") == type);
        CHECK(report.at("actions")[0].at("accepted") == true);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "a fork split outside playable placement keeps independent branch cohorts",
          "[balance][controller][fork][fallback]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto level = config::Json::parse(R"JSON({
      "schema":2,"name":"offscreen_fork_controller","region":"capillary",
      "world":{"min":[0,0],"max":[180,100],"cell_size":1},
      "vessels":[
        {"id":"upper","points":[{"p":[-40,50],"w":18},{"p":[-20,80],"w":18},{"p":[150,80],"w":18},{"p":[176,50],"w":18}]},
        {"id":"lower","points":[{"p":[-40,50],"w":18},{"p":[-20,20],"w":18},{"p":[150,20],"w":18},{"p":[176,50],"w":18}]}],
      "spawn_points":[{"id":"p0","pos":[-40,50],"radius":2,"lane_id":"upper"}],
      "objectives":[{"id":"organ","pos":[176,50],"half_extents":[4,15]}],
      "placement_zones":[{"min":[2,2],"max":[178,98]}],
      "squad_paths":[
        {"id":"upper_route","lane_id":"upper","points":[[-40,50],[-20,80],[150,80],[176,50]],"half_width":3},
        {"id":"lower_route","lane_id":"lower","points":[[-40,50],[-20,20],[150,20],[176,50]],"half_width":3}],
      "waves":[{"name":"two_branches","prep_time":12,"spawns":[
        {"family":"virus","count":30,"duration":1,"spawn_point_id":"p0","squad_paths":["upper_route","lower_route"]}]}]
    })JSON");
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 7133},
        {"max_ticks", 20}, {"opening_atp", 300},
        {"controller", {{"policy", "specialist"}, {"weights", {{"neutrophil", 1}}},
                        {"reaction_ticks", 1}, {"prep_ticks", 1000}, {"use_abilities", false}}}};
    const auto report = execute(spec, base);
    INFO(report.at("diagnostics").dump());
    u32 upper = 0, lower = 0;
    for (const auto& action : report.at("actions")) {
        if (action.at("kind") != "deploy") continue;
        REQUIRE(action.at("accepted") == true);
        if (action["position"][1].get<f32>() > 50.0f) ++upper;
        else ++lower;
    }
    CHECK(upper >= 8);
    CHECK(lower >= 8);
    CHECK(std::max(upper, lower) - std::min(upper, lower) <= 3);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "undefended balance policy starts authored waves without free defenses",
          "[balance][controller][waves]") {
    ControllerFixture fixture;
    fixture.init();
    ControllerParams params;
    params.policy = Policy::None;
    params.reaction_ticks = 1;
    params.prep_ticks = 2;
    BalanceController controller(params);
    CHECK(controller.decide(fixture.view()).empty());
    fixture.world.tick();
    CHECK(controller.decide(fixture.view()).empty());
    fixture.world.tick();
    const auto actions = controller.decide(fixture.view());
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == ActionKind::StartWave);
    CHECK(fixture.world.swarmers().count() == 0);
    CHECK(fixture.economy.snapshot().total_spent == 0);
    fixture.waves.request_early_start();
    fixture.waves.tick(fixture.world, fixture.world.rng(), kFixedDt);
    CHECK(fixture.waves.status().phase == WavePhase::Spawning);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller starts a funded wave once the experiment spending cap is exhausted",
          "[balance][controller][waves][economy]") {
    ControllerFixture fixture;
    fixture.init();
    ControllerParams params;
    params.policy = Policy::Competent;
    params.reaction_ticks = 1;
    params.prep_ticks = 1;
    BalanceController controller(params);
    auto view = fixture.view();
    view.spending_remaining = 0;
    CHECK(controller.decide(view).empty());
    fixture.world.tick();
    const auto actions = controller.decide(view);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == ActionKind::StartWave);
    CHECK(fixture.economy.atp() == 300);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance controller uses unlocked emergency abilities only on visible danger",
          "[balance][controller][abilities]") {
    ControllerFixture fixture;
    fixture.init(0);
    ControllerParams params;
    params.policy = Policy::Tactical;
    params.weights.fill(0.0f);
    BalanceController controller(params);
    fixture.abilities.set_unlocked(1u << static_cast<u32>(AbilityId::HistamineFlare));
    fixture.waves.request_early_start();
    fixture.waves.tick(fixture.world, fixture.world.rng(), kFixedDt);
    SECTION("empty board does not waste a cast") {
        const auto actions = controller.decide(fixture.view());
        CHECK(std::none_of(actions.begin(), actions.end(), [](const Action& action) {
            return action.kind == ActionKind::CastAbility;
        }));
    }
    SECTION("hidden horde does not trigger a cast") {
        fixture.spawn(Vec2{100.0f, 50.0f}, 60, true);
        CHECK(controller.decide(fixture.view()).empty());
    }
    SECTION("ability ablation preserves unlocks while producing no casts") {
        params.use_abilities = false;
        BalanceController ablated(params);
        fixture.spawn(Vec2{100.0f, 50.0f}, 60);
        CHECK(ablated.decide(fixture.view()).empty());
        CHECK(fixture.abilities.unlocked(AbilityId::HistamineFlare));
        CHECK(fixture.abilities.ready(AbilityId::HistamineFlare));
    }
    SECTION("visible horde produces a reproducible legal cast intent") {
        fixture.spawn(Vec2{100.0f, 50.0f}, 60);
        const auto actions = controller.decide(fixture.view());
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].kind == ActionKind::CastAbility);
        CHECK(actions[0].ability == AbilityId::HistamineFlare);
        CHECK(fixture.world.desc().world_bounds.contains(actions[0].position));
        CHECK(fixture.abilities.ready(actions[0].ability));
        REQUIRE(fixture.abilities.cast(fixture.world, actions[0].ability, actions[0].position));
        CHECK_FALSE(fixture.abilities.ready(actions[0].ability));
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance policy names round trip and reject unknown input", "[balance][controller]") {
    for (Policy policy : {Policy::None, Policy::Novice, Policy::Competent, Policy::Tactical, Policy::Specialist}) {
        Policy parsed = Policy::None;
        REQUIRE(parse_policy(policy_name(policy), parsed));
        CHECK(parsed == policy);
    }
    Policy unchanged = Policy::Tactical;
    CHECK_FALSE(parse_policy("omniscient", unchanged));
    CHECK(unchanged == Policy::Tactical);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "earned level three controller saves for a needed role instead of buying surplus cheap cells",
          "[balance][controller][campaign][economy][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_03_two_chambers.json"));
    REQUIRE(text.has_value());
    const u32 cheap = base.towers.stats[static_cast<u32>(TowerType::CytotoxicT)].build_cost;
    const u32 needed = base.towers.stats[static_cast<u32>(TowerType::Neutrophil)].build_cost;
    REQUIRE(cheap < needed);
    // Actual earned pre-L3 offense snapshot from the retry journey. This
    // regression isolates the observed cheap-cell reinvestment trap on its
    // real map; it does not grant a stronger build or change enemy stats.
    const auto earned = config::Json::parse(R"JSON({
      "version":3,"memory_cells":9,"antibodies":0,
      "spent_memory_cells":927,"spent_antibodies":2,
      "progress":{"levels_completed":2,"completed_level_ids":["campaign_01_first_bend","campaign_02_island_climb"],"unlocked_regions":[]},
      "tree":{"cytotoxic.unlock":1,"hub.bone_marrow_reserve":1,"hub.efficient_clearance":1,
       "neutrophil.accuracy":2,"neutrophil.aggro_range":3,"neutrophil.capstone":1,
       "neutrophil.round_damage":5,"neutrophil.squad_size":3,"neutrophil.trigger_rate":4,"neutrophil.vitality":4}
    })JSON");
    u32 wallet = cheap;
    bool cap_blocks_needed = false;
    bool funded = false;
    SECTION("bank a small income payment instead of another unwanted latcher") {}
    SECTION("the next few ATP fund the underrepresented shooter") { wallet = needed; funded = true; }
    SECTION("an exhausted experiment cap does not reserve an impossible purchase") { cap_blocks_needed = true; }
    std::array<Vec2, 2> holding{};
    usize holds = 0;
    const config::Json setup = {{"schema", 1}, {"level_document", config::Json::parse(*text)},
        {"max_ticks", 1}, {"progression_snapshot", earned}, {"actions", config::Json::array()}};
    const auto observed_setup = execute(setup, base, [&](const CaptureView& view) {
        REQUIRE(view.level.spawn_points.size() == 2);
        for (const auto& spawn : view.level.spawn_points) {
            const auto traffic = navigation_path(view.world, spawn.position);
            REQUIRE(traffic["complete"] == true);
            bool found = false;
            for (const auto& point : traffic["points"]) {
                const Vec2 position{point[0].get<f32>(), point[1].get<f32>()};
                if (!view.world.desc().world_bounds.contains(position)) continue;
                if (!view.towers.validate_deploy(view.world, TowerType::CytotoxicT,
                    position, std::numeric_limits<u32>::max()).valid()) continue;
                holding[holds++] = position;
                found = true;
                break;
            }
            REQUIRE(found);
        }
    });
    REQUIRE(observed_setup["summary"]["valid"] == true);
    REQUIRE(holds == 2);
    const config::Json spec = {{"schema", 1}, {"level_document", config::Json::parse(*text)}, {"seed", 31022},
        {"max_ticks", 1}, {"sample_ticks", 1}, {"opening_atp", 20 * cheap + wallet},
        {"progression_snapshot", earned}, {"allowed_cells", {"neutrophil", "cytotoxic_t"}},
        {"actions", config::Json::array({
            {{"tick", 0}, {"kind", "deploy"}, {"tower", "cytotoxic_t"}, {"position", {holding[0].x, holding[0].y}}, {"quantity", 10}},
            {{"tick", 0}, {"kind", "deploy"}, {"tower", "cytotoxic_t"}, {"position", {holding[1].x, holding[1].y}}, {"quantity", 10}}})}};
    bool captured = false;
    const auto report = execute(spec, base, [&](const CaptureView& view) {
        captured = true;
        REQUIRE(view.world.swarmers().count() == 20);
        REQUIRE(view.economy.atp() == wallet);
        ControllerParams params;
        params.policy = Policy::Tactical;
        params.use_abilities = false;
        BalanceController controller(params);
        auto observed = ControllerView{view.world, view.level, view.towers, view.economy, view.waves, view.abilities};
        if (cap_blocks_needed) observed.spending_remaining = cheap;
        const auto hash = view.world.state_hash();
        const auto actions = controller.decide(observed);
        const auto* action = deploy_action(actions);
        if (funded || cap_blocks_needed) {
            REQUIRE(action != nullptr);
            CHECK(action->tower == (funded ? TowerType::Neutrophil : TowerType::CytotoxicT));
            CHECK(view.towers.validate_deploy(view.world, action->tower, action->position, wallet).valid());
        } else CHECK(action == nullptr);
        CHECK(view.world.state_hash() == hash);
        CHECK(view.economy.atp() == wallet);
    });
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(captured);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "another front's shooters cannot satisfy an empty approach's composition",
          "[balance][controller][routes][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("balance/levels/two_fronts.json"));
    REQUIRE(text.has_value());
    const u32 cost = base.towers.stats[static_cast<u32>(TowerType::Neutrophil)].build_cost;
    const config::Json spec = {{"schema", 1}, {"level_document", config::Json::parse(*text)}, {"seed", 72011},
        {"max_ticks", 1}, {"opening_atp", 20 * cost + 100},
        {"progression", {{"grant_tree", {{"neutrophil.round_damage", 1}, {"macrophage.unlock", 1}}}}},
        {"allowed_cells", {"neutrophil", "macrophage"}},
        {"actions", config::Json::array({
            {{"tick", 0}, {"kind", "deploy"}, {"tower", "neutrophil"}, {"position", {80, 135}}, {"quantity", 10}},
            {{"tick", 0}, {"kind", "deploy"}, {"tower", "neutrophil"}, {"position", {80, 135}}, {"quantity", 10}}})}};
    bool captured = false;
    const auto report = execute(spec, base, [&](const CaptureView& view) {
        captured = true;
        REQUIRE(view.world.swarmers().count() == 20);
        ControllerParams params;
        params.policy = Policy::Tactical;
        params.use_abilities = false;
        BalanceController controller(params);
        const auto actions = controller.decide({view.world, view.level, view.towers, view.economy, view.waves, view.abilities});
        const auto* action = deploy_action(actions);
        REQUIRE(action != nullptr);
        CHECK(action->tower == TowerType::Neutrophil);
        CHECK(path_distance(action->position, config::Json::parse(*text)["squad_paths"][1]) <= 20.0f);
        CHECK(view.towers.validate_deploy(view.world, action->tower, action->position, view.economy.atp()).valid());
    });
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(captured);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "earned core breach controller clears the real first wave through paid automatic play",
          "[balance][controller][campaign][junction][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_10_core_breach.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    level["waves"].erase(level["waves"].begin() + 1, level["waves"].end());
    level["name"] = "diagnostic_actual_control_l10_firstwave";
    // Actual first post-L9 V9 control snapshot, before seeing any L10 outcome.
    // Later waves are omitted; the exact first wave and geometry stay intact.
    const auto earned = config::Json::parse(R"JSON({
      "version":3,"memory_cells":2,"antibodies":0,"spent_memory_cells":3027,"spent_antibodies":9,
      "progress":{"levels_completed":9,"unlocked_regions":[],"completed_level_ids":[
        "campaign_01_first_bend","campaign_02_island_climb","campaign_03_two_chambers",
        "campaign_04_twin_channels","campaign_05_ring_road","campaign_06_circuit_board",
        "campaign_07_chevron_sieve","campaign_08_blob_field","campaign_09_funnel_walls"]},
      "tree":{"ability.clot.unlock":1,"ability.complement.unlock":1,"ability.fever.unlock":1,
        "ability.histamine.unlock":1,"cytotoxic.attach_speed":1,"cytotoxic.drain":1,
        "cytotoxic.search":1,"cytotoxic.stamina":1,"cytotoxic.unlock":1,"fibroblast.unlock":1,
        "goblet.slow_duration":1,"goblet.slow_strength":1,"goblet.splash_radius":1,
        "goblet.tower_health":1,"goblet.unlock":1,"goblet.weakness":1,
        "hub.bone_marrow_reserve":5,"hub.efficient_clearance":5,"hub.elite_response":4,
        "hub.field_requisition":3,"hub.homeostasis":4,"hub.membrane_resilience":3,
        "hub.systemic_potency":3,"macrophage.arms":1,"macrophage.captives":3,
        "macrophage.grab_speed":4,"macrophage.health":4,"macrophage.search":2,
        "macrophage.unlock":1,"macrophage.wall":2,"neutrophil.accuracy":3,
        "neutrophil.aggro_range":3,"neutrophil.capstone":1,"neutrophil.round_damage":5,
        "neutrophil.squad_size":3,"neutrophil.trigger_rate":4,"neutrophil.vitality":4}
    })JSON");
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 31029},
        {"max_ticks", 7200}, {"progression_snapshot", earned},
        {"controller", {{"policy", "tactical"}}}};
    const auto report = execute(spec, base);
    INFO(report.at("diagnostics").dump());
    INFO(report.at("summary").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(report.at("summary").at("status") == "cleared");
    CHECK(report.at("summary").at("waves_cleared") == 1);
    CHECK(report.at("summary").at("objective_integrity").get<f32>() > 0.0f);
    CHECK(report.at("state").at("alive") == 0);
    REQUIRE(report.at("economy").at("opening_atp") == 950);
    CHECK(report.at("summary").at("actions_rejected") == 0);
    CHECK(report.at("economy").at("spent").get<u32>() > 950);
    CHECK_FALSE(report.at("spec").contains("actions"));
    for (const auto& item : report["combat"]["pools"].items()) CHECK(item.value()["denied"] == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "blob field pooling preserves public squad alternative coverage",
          "[balance][controller][campaign][junction][routes][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_08_blob_field.json"));
    REQUIRE(text.has_value());
    auto level = config::Json::parse(*text);
    SECTION("all four alternatives are active now") {}
    SECTION("unused public alternatives still protect a later branch switch") {
        auto& spawns = level["waves"][0]["spawns"];
        for (usize i = spawns.size(); i-- > 0;) {
            const auto id = spawns[i]["squad_paths"][0].get<std::string>();
            if (id == "upper_island_route" || id == "cross_island_route")
                spawns.erase(spawns.begin() + i);
        }
        REQUIRE(spawns.size() == 2);
    }
    // This actual first-entry V11 control profile exposed a false common
    // pocket: successful pure-flow samples chose the lower branch, while
    // current squads also took the upper/cross alternatives around the island.
    const auto earned = config::Json::parse(R"JSON({
      "version":3,"memory_cells":13,"antibodies":0,"spent_memory_cells":1921,"spent_antibodies":7,
      "progress":{"levels_completed":7,"unlocked_regions":[],"completed_level_ids":[
        "campaign_01_first_bend","campaign_02_island_climb","campaign_03_two_chambers",
        "campaign_04_twin_channels","campaign_05_ring_road","campaign_06_circuit_board",
        "campaign_07_chevron_sieve"]},
      "tree":{"ability.complement.unlock":1,"ability.fever.unlock":1,"ability.histamine.unlock":1,
        "cytotoxic.unlock":1,"goblet.unlock":1,"hub.bone_marrow_reserve":5,
        "hub.efficient_clearance":5,"hub.elite_response":3,"hub.field_requisition":3,
        "hub.homeostasis":2,"hub.systemic_potency":3,"macrophage.health":1,
        "macrophage.unlock":1,"neutrophil.accuracy":3,"neutrophil.aggro_range":3,
        "neutrophil.capstone":1,"neutrophil.round_damage":5,"neutrophil.squad_size":3,
        "neutrophil.trigger_rate":4,"neutrophil.vitality":4}
    })JSON");
    config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 31022},
        {"max_ticks", 20}, {"progression_snapshot", earned},
        {"controller", {{"policy", "tactical"}, {"reaction_ticks", 1},
                        {"prep_ticks", 1000}, {"use_abilities", false}}}};
    // Tactical's limited opening sample need not buy every support role.
    // A separate, normally paid specialist arm exercises Goblet geometry
    // without making a particular composition an artificial success rule.
    for (const bool goblet_only : {false, true}) {
        if (goblet_only) {
            spec["controller"]["policy"] = "specialist";
            spec["controller"]["weights"] = {{"goblet_cell", 1}};
        }
        const auto report = execute(spec, base);
        INFO(report.at("diagnostics").dump());
        REQUIRE(report.at("summary").at("valid") == true);
        REQUIRE(report.at("summary").at("actions_rejected") == 0);
        std::array<u32, kTowerTypeCount> purchases{};
        for (const auto& action : report.at("actions")) {
            if (action.at("kind") != "deploy") continue;
            REQUIRE(action.at("accepted") == true);
            TowerType type{};
            REQUIRE(parse_tower_type(action.at("tower").get<std::string>(), type));
            ++purchases[static_cast<u32>(type)];
            const auto profile = swarmer_profile(type);
            const f32 reach = profile.kind == sim::SwarmerKind::Shooter ||
                profile.kind == sim::SwarmerKind::ArborGrabber ? profile.attach_radius : profile.search_radius;
            const Vec2 position{action["position"][0].get<f32>(), action["position"][1].get<f32>()};
            bool covers_an_approach = false;
            for (const auto& spawn : level.at("spawn_points")) {
                bool covers = true;
                for (const auto& path : level.at("squad_paths")) {
                    if (path.at("points")[0] != spawn.at("pos")) continue;
                    if (path_distance(position, path) > reach) covers = false;
                }
                if (covers) covers_an_approach = true;
            }
            INFO(action.dump());
            CHECK(covers_an_approach);
        }
        if (goblet_only) CHECK(purchases[static_cast<u32>(TowerType::GobletCell)] > 0);
        else {
            CHECK(purchases[static_cast<u32>(TowerType::Neutrophil)] > 0);
            CHECK(purchases[static_cast<u32>(TowerType::Macrophage)] > 0);
            CHECK(purchases[static_cast<u32>(TowerType::CytotoxicT)] > 0);
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "parallel authored approaches prepare before broad circuit chambers",
          "[balance][controller][campaign][routes][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto text = platform::read_text_file(platform::asset_path("levels/campaign_06_circuit_board.json"));
    REQUIRE(text.has_value());
    const auto level = config::Json::parse(*text);
    // Actual V14 first-entry control profile, with ordinary opening resources.
    // Only the preparation horizon is bounded; no scripted purchases, grants,
    // altered cell costs or enemy inputs establish this placement regression.
    const auto earned = config::Json::parse(R"JSON({
      "version":3,"memory_cells":8,"antibodies":0,"spent_memory_cells":1876,"spent_antibodies":5,
      "progress":{"levels_completed":5,"unlocked_regions":[],"completed_level_ids":[
        "campaign_01_first_bend","campaign_02_island_climb","campaign_03_two_chambers",
        "campaign_04_twin_channels","campaign_05_ring_road"]},
      "tree":{"ability.fever.unlock":1,"ability.histamine.unlock":1,"goblet.unlock":1,
        "hub.bone_marrow_reserve":5,"hub.efficient_clearance":2,"hub.elite_response":2,
        "hub.field_requisition":2,"hub.homeostasis":1,"hub.systemic_potency":1,
        "macrophage.arms":1,"macrophage.captives":3,"macrophage.grab_speed":4,
        "macrophage.health":4,"macrophage.search":1,"macrophage.unlock":1,
        "neutrophil.accuracy":2,"neutrophil.aggro_range":3,"neutrophil.capstone":1,
        "neutrophil.round_damage":5,"neutrophil.squad_size":3,"neutrophil.trigger_rate":4,
        "neutrophil.vitality":4}
    })JSON");
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 31026},
        {"max_ticks", 180}, {"progression_snapshot", earned},
        {"controller", {{"policy", "tactical"}, {"use_abilities", false}}}};
    const auto report = execute(spec, base);
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(report.at("summary").at("actions_rejected") == 0);
    u32 upper = 0, lower = 0;
    for (const auto& action : report.at("actions")) {
        if (action.at("kind") != "deploy") continue;
        REQUIRE(action.at("accepted") == true);
        const Vec2 position{action["position"][0].get<f32>(), action["position"][1].get<f32>()};
        TowerType type{};
        REQUIRE(parse_tower_type(action.at("tower").get<std::string>(), type));
        const auto profile = swarmer_profile(type);
        const f32 reach = profile.kind == sim::SwarmerKind::Shooter ||
            profile.kind == sim::SwarmerKind::ArborGrabber ? profile.attach_radius : profile.search_radius;
        const bool is_upper = position.y > 120.0f;
        const auto& spawn = level.at("spawn_points")[is_upper ? 0 : 2];
        for (const auto& path : level.at("squad_paths")) {
            if (path.at("points")[0] == spawn.at("pos")) CHECK(path_distance(position, path) <= reach);
        }
        // Public feeder geometry: upper widens beyond x65, the lower long
        // feeder enters its broad chamber beyond y30. The failed controller
        // instead left idle cohorts at (109,173) and (191,85) after those mouths.
        INFO(action.dump());
        if (is_upper) { ++upper; CHECK(position.x < 65.0f); }
        else { ++lower; CHECK(position.y < 30.0f); }
    }
    CHECK(upper >= 4);
    CHECK(lower >= 4);
    CHECK(report.at("economy").at("spent").get<u32>() > 0);
    CHECK_FALSE(report.at("spec").contains("actions"));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "last-second convergence keeps independent upstream approach cohorts",
          "[balance][controller][junction][routes][regression]") {
    config::ConfigStore store;
    GameConfig base;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
    const auto level = config::Json::parse(R"JSON({
      "schema":2,"name":"late_junction_controller","region":"capillary",
      "world":{"min":[0,0],"max":[180,100],"cell_size":1},
      "vessels":[
        {"id":"upper","points":[{"p":[-20,85],"w":18},{"p":[170,85],"w":18},{"p":[170,50],"w":18},{"p":[174,50],"w":18}]},
        {"id":"lower","points":[{"p":[-20,15],"w":18},{"p":[170,15],"w":18},{"p":[170,50],"w":18},{"p":[174,50],"w":18}]}],
      "spawn_points":[{"id":"p0","pos":[-20,85],"radius":2,"lane_id":"upper"},
        {"id":"p1","pos":[-20,15],"radius":2,"lane_id":"lower"}],
      "objectives":[{"id":"organ","pos":[174,50],"half_extents":[4,10]}],
      "placement_zones":[{"min":[2,2],"max":[178,98]}],
      "squad_paths":[
        {"id":"upper_route","lane_id":"upper","points":[[-20,85],[170,85],[170,50],[174,50]],"half_width":3},
        {"id":"lower_route","lane_id":"lower","points":[[-20,15],[170,15],[170,50],[174,50]],"half_width":3}],
      "waves":[{"name":"two_entries","prep_time":12,"spawns":[
        {"family":"virus","count":30,"duration":1,"spawn_point_id":"p0","squad_paths":["upper_route"]},
        {"family":"virus","count":30,"duration":1,"spawn_point_id":"p1","squad_paths":["lower_route"]}]}]
    })JSON");
    const u32 cost = base.towers.stats[static_cast<u32>(TowerType::Neutrophil)].build_cost;
    const config::Json spec = {{"schema", 1}, {"level_document", level}, {"seed", 4133},
        {"max_ticks", 20}, {"opening_atp", 20 * cost},
        {"controller", {{"policy", "specialist"}, {"weights", {{"neutrophil", 1}}},
                        {"reaction_ticks", 1}, {"prep_ticks", 1000}, {"use_abilities", false}}}};
    const auto report = execute(spec, base);
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    u32 upper = 0, lower = 0;
    for (const auto& action : report.at("actions")) {
        if (action.at("kind") != "deploy") continue;
        REQUIRE(action.at("accepted") == true);
        const Vec2 position{action["position"][0].get<f32>(), action["position"][1].get<f32>()};
        CHECK(position.x < 140.0f);
        CHECK(path_remaining(position, level["squad_paths"][position.y > 50.0f ? 0 : 1]) >= 30.0f);
        if (position.y > 50.0f) ++upper;
        else ++lower;
    }
    CHECK(upper >= 8);
    CHECK(lower >= 8);
    CHECK(std::max(upper, lower) - std::min(upper, lower) <= 3);
}
