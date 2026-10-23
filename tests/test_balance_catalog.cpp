#include "BalanceTestIsolation.h"
#include "game/balance/BalanceCatalog.h"
#include "config/Registry.h"
#include "game/meta/ImmunityTree.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace immune;
using namespace immune::game;
using immune::config::Json;

namespace {
GameConfig catalog_config() {
    config::ConfigStore store;
    GameConfig config;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), config, error));
    return config;
}
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance catalog describes every real tree rank price parent and capability",
          "[balance][catalog]") {
    const auto config = catalog_config();
    const auto catalog = balance::describe_catalog(config);
    REQUIRE(catalog["tree"]["nodes"].size() == kTreeNodeCount);
    CHECK(catalog["tree"]["capstone_threshold"] == 0);
    CHECK(catalog["tree"]["root"] == tree_node(kTreeRoot).key);
    std::set<std::string> keys;
    for (const auto& item : catalog["tree"]["nodes"]) {
        const auto node = static_cast<TreeNode>(item["id"].get<u32>());
        const auto& def = tree_node(node);
        CAPTURE(def.key);
        CHECK(item["key"] == def.key);
        CHECK(keys.insert(item["key"].get<std::string>()).second);
        CHECK(item["max_rank"] == def.max_level);
        CHECK(item["effect"] == def.effect);
        if (def.parent == TreeNode::Count) CHECK(item["parent"].is_null());
        else CHECK(item["parent"] == tree_node(def.parent).key);
        REQUIRE(item["rank_prices"].size() == def.max_level);
        u64 cumulative_mc = 0, cumulative_ab = 0;
        for (u32 rank = 1; rank <= def.max_level; ++rank) {
            const auto price = tree_node_cost(config.meta, node, static_cast<u8>(rank - 1));
            cumulative_mc += price.memory_cells;
            cumulative_ab += price.antibodies;
            const auto& advertised = item["rank_prices"][rank - 1];
            CHECK(advertised["rank"] == rank);
            CHECK(advertised["memory_cells"] == price.memory_cells);
            CHECK(advertised["antibodies"] == price.antibodies);
            CHECK(advertised["cumulative_memory_cells"] == cumulative_mc);
            CHECK(advertised["cumulative_antibodies"] == cumulative_ab);
        }
    }
    REQUIRE(catalog["cells"].size() == kTowerTypeCount);
    REQUIRE(catalog["abilities"].size() == kAbilityCount);
    for (const auto& cell : catalog["cells"]) {
        const auto type = static_cast<TowerType>(cell["id"].get<u32>());
        CHECK(cell["key"] == tower_type_name(type));
        if (tower_root(type) == TreeNode::Count) CHECK(cell["root"].is_null());
        else CHECK(cell["root"] == tree_node(tower_root(type)).key);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance catalog exposes every loaded registry value and documentation without mutation",
          "[balance][catalog][config]") {
    auto config = catalog_config();
    config.economy.starting_atp = 731;
    config::Registry registry;
    bind_game_config(registry, config);
    const auto before = dump_game_config(config);
    const auto catalog = balance::describe_catalog(config);
    CHECK(dump_game_config(config) == before);
    const auto paths = registry.field_paths();
    REQUIRE(catalog["registry"]["fields"].size() == paths.size());
    for (usize i = 0; i < paths.size(); ++i) {
        const auto& item = catalog["registry"]["fields"][i];
        std::string value, error;
        REQUIRE(registry.get(paths[i], value, error));
        CHECK(item["path"] == paths[i]);
        CHECK(item["value"] == value);
        CHECK(item["documentation"] == std::string(registry.doc(paths[i])));
    }
    CHECK(catalog["base_config"] == Json(before));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance catalog reports actual rank effects against the shipped roster",
          "[balance][catalog][meta]") {
    const auto config = catalog_config();
    const auto catalog = balance::describe_catalog(config);
    for (const auto& node : catalog["tree"]["nodes"]) {
        const std::string key = node["key"];
        if (key == "hub.elite_response" && config.enemies.elites.empty()) {
            CHECK(node["applicability"]["status"] == "available");
            for (u32 t = 0; t < kTowerTypeCount; ++t) {
                const std::string path = std::string("towers.") + tower_type_name(static_cast<TowerType>(t)) + ".swarm.max_health";
                bool found = false;
                for (const auto& change : node["rank_effects"][0]["config_changes"])
                    if (change["path"] == path) found = true;
                CHECK(found);
            }
        }
        if (key == "hub.bone_marrow_reserve") {
            bool found = false;
            for (const auto& change : node["rank_effects"][0]["config_changes"])
                if (change["path"] == "economy.starting_atp") found = true;
            CHECK(found);
        }
        if (key == "cytotoxic.capstone") {
            const auto& rules = node["rank_effects"][0]["rules"]["unit_capstones"]["cytotoxic_t"];
            CHECK(rules["kill_pulse_radius"].get<f32>() > 0.0f);
            CHECK(rules["named_damage_mult"].get<f32>() > 1.0f);
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance catalog exposes real cluster intake capacity and the engine ceiling", "[balance][catalog]") {
    auto config = catalog_config();
    for (const u32 configured : {1u,sim::kArborMaxCaptives+1u}) {
        config.towers.mechanics[static_cast<u32>(TowerType::Macrophage)].arbor_grabber.max_captives = configured;
        TowerSystem towers;
        apply_tower_config(towers,config.towers);
        const auto actual = swarmer_profile(TowerType::Macrophage).arbor_max_captives;
        const auto catalog = balance::describe_catalog(config);
        bool found = false;
        for (const auto& item : catalog["tree"]["nodes"]) {
            if (item["key"] != "macrophage.arms") continue;
            found = true;
            CHECK(item["runtime_constraint"]["configured_baseline"] == configured);
            CHECK(item["runtime_constraint"]["runtime_baseline"] == actual);
            CHECK(item["runtime_constraint"]["engine_ceiling"] == sim::kArborMaxCaptives);
            CHECK(item["applicability"]["status"] == "available");
            GameConfig upgraded = config;
            TreeLevels levels;
            levels[TreeNode::MacrophageArms] = 1;
            apply_immunity_tree(levels, upgraded);
            apply_tower_config(towers, upgraded.towers);
            CHECK(item["rank_effects"][0]["runtime_max_captives"] == swarmer_profile(TowerType::Macrophage).arbor_max_captives);
        }
        CHECK(found);
    }
}
