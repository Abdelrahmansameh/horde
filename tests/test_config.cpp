// tests/test_config.cpp — the config machinery: strict parsing, schema-driven
// round-trip, path addressing, and the file store.
//
// These exercise the generic layer against a toy schema. The per-system schemas
// (towers, enemies, waves, sim, economy, abilities, meta) get their own cases
// once they land; what is proven here is that a dumped file always loads back
// identically and that a typo can never be a silent no-op.
#include "config/ConfigStore.h"
#include "config/Field.h"
#include "config/Json.h"
#include "config/Registry.h"
#include "game/config/GameConfig.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace immune;
using immune::config::Ctx;
using immune::config::Json;

namespace {

enum class Flavor : u8 { Sweet = 0, Sour = 1, Bitter = 2 };

struct Toy {
    f32 speed = 0.0f;
    u32 cost = 0;
    i32 offset = 0;
    u8 tier = 0;
    bool enabled = false;
    Vec2 drift{0.0f, 0.0f};
    Vec4 tint{0.0f, 0.0f, 0.0f, 0.0f};
    std::string label;
    Flavor flavor = Flavor::Sweet;
};

IMMUNE_CONFIG_SCHEMA_ASSERT(Toy);

constexpr config::EnumEntry kFlavorValues[] = {
    {"sweet", static_cast<i64>(Flavor::Sweet)},
    {"sour", static_cast<i64>(Flavor::Sour)},
    {"bitter", static_cast<i64>(Flavor::Bitter)},
    {nullptr, 0},
};

const config::Field kToyFields[] = {
    IMMUNE_CONFIG_FIELD(Toy, speed, config::FieldKind::F32, "units per second"),
    IMMUNE_CONFIG_FIELD(Toy, cost, config::FieldKind::U32, "ATP"),
    IMMUNE_CONFIG_FIELD(Toy, offset, config::FieldKind::I32, ""),
    IMMUNE_CONFIG_FIELD(Toy, tier, config::FieldKind::U8, ""),
    IMMUNE_CONFIG_FIELD(Toy, enabled, config::FieldKind::Bool, ""),
    IMMUNE_CONFIG_FIELD(Toy, drift, config::FieldKind::Vec2, ""),
    IMMUNE_CONFIG_FIELD(Toy, tint, config::FieldKind::Vec4, ""),
    IMMUNE_CONFIG_FIELD(Toy, label, config::FieldKind::String, ""),
    IMMUNE_CONFIG_ENUM_FIELD(Toy, flavor, config::FieldKind::EnumU8, "", kFlavorValues),
};

const config::Schema kToySchema{"toy", kToyFields};

const char* kValidToy = R"JSON({
  "speed": 12.5,
  "cost": 70,
  "offset": -3,
  "tier": 2,
  "enabled": true,
  "drift": [0.5, 0.28],
  "tint": [0.2, 0.8, 0.9, 0.5],
  "label": "macrophage",
  "flavor": "bitter"
})JSON";

Toy parse_toy(const char* text, const char* root = "toy.json") {
    Toy toy;
    Ctx ctx(root);
    config::parse_struct(Json::parse(text), kToySchema, &toy, ctx);
    return toy;
}

/// Captures the message of the std::runtime_error a parse is expected to throw.
std::string parse_error(const char* text) {
    try {
        (void)parse_toy(text);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::filesystem::path scratch_dir(const char* leaf) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "immune_config_test" / leaf;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("config parses every field kind", "[config][parse]") {
    const Toy toy = parse_toy(kValidToy);
    REQUIRE(toy.speed == 12.5f);
    REQUIRE(toy.cost == 70u);
    REQUIRE(toy.offset == -3);
    REQUIRE(toy.tier == 2u);
    REQUIRE(toy.enabled);
    REQUIRE(toy.drift.x == 0.5f);
    REQUIRE(toy.drift.y == 0.28f);
    REQUIRE(toy.tint.w == 0.5f);
    REQUIRE(toy.label == "macrophage");
    REQUIRE(toy.flavor == Flavor::Bitter);
}

TEST_CASE("config rejects a missing field and names its path", "[config][parse]") {
    const char* missing = R"JSON({
      "speed": 12.5, "cost": 70, "offset": -3, "tier": 2, "enabled": true,
      "drift": [0.5, 0.28], "tint": [0.2, 0.8, 0.9, 0.5], "flavor": "sweet"
    })JSON";
    const std::string err = parse_error(missing);
    REQUIRE(contains(err, "toy.json"));
    REQUIRE(contains(err, "missing required field"));
    REQUIRE(contains(err, "'label'"));
}

TEST_CASE("config rejects an unknown key and suggests the intended one",
          "[config][parse]") {
    const char* typo = R"JSON({
      "sped": 12.5, "speed": 1.0, "cost": 70, "offset": -3, "tier": 2,
      "enabled": true, "drift": [0.5, 0.28], "tint": [0.2, 0.8, 0.9, 0.5],
      "label": "x", "flavor": "sweet"
    })JSON";
    const std::string err = parse_error(typo);
    REQUIRE(contains(err, "unknown field 'sped'"));
    REQUIRE(contains(err, "did you mean 'speed'?"));
}

TEST_CASE("config rejects wrong types rather than coercing", "[config][parse]") {
    const char* fractional_int = R"JSON({
      "speed": 1.0, "cost": 70.5, "offset": -3, "tier": 2, "enabled": true,
      "drift": [0.5, 0.28], "tint": [0.2, 0.8, 0.9, 0.5], "label": "x",
      "flavor": "sweet"
    })JSON";
    REQUIRE(contains(parse_error(fractional_int), "must be a whole number"));

    const char* short_vec = R"JSON({
      "speed": 1.0, "cost": 70, "offset": -3, "tier": 2, "enabled": true,
      "drift": [0.5], "tint": [0.2, 0.8, 0.9, 0.5], "label": "x",
      "flavor": "sweet"
    })JSON";
    REQUIRE(contains(parse_error(short_vec), "array of 2 numbers"));

    const char* bad_enum = R"JSON({
      "speed": 1.0, "cost": 70, "offset": -3, "tier": 2, "enabled": true,
      "drift": [0.5, 0.28], "tint": [0.2, 0.8, 0.9, 0.5], "label": "x",
      "flavor": "salty"
    })JSON";
    const std::string err = parse_error(bad_enum);
    REQUIRE(contains(err, "unknown value 'salty'"));
    REQUIRE(contains(err, "sweet, sour, bitter"));
}

TEST_CASE("config dump round-trips to identical values", "[config][dump]") {
    const Toy original = parse_toy(kValidToy);

    Json dumped = Json::object();
    config::dump_struct(dumped, kToySchema, &original);

    Toy reloaded;
    Ctx ctx("toy.json");
    config::parse_struct(dumped, kToySchema, &reloaded, ctx);

    Json redumped = Json::object();
    config::dump_struct(redumped, kToySchema, &reloaded);

    // Byte-stable, not merely value-equal: the dumped file is what gets
    // committed, so a second dump must not produce a spurious diff.
    REQUIRE(dumped.dump(2) == redumped.dump(2));
    REQUIRE(reloaded.speed == original.speed);
    REQUIRE(reloaded.label == original.label);
    REQUIRE(reloaded.flavor == original.flavor);
}

TEST_CASE("registry addresses every field kind by path", "[config][registry]") {
    Toy toy = parse_toy(kValidToy);
    config::Registry registry;
    registry.bind("towers.macrophage.2.stats", kToySchema, &toy);

    std::string value;
    std::string err;

    REQUIRE(registry.get("towers.macrophage.2.stats.cost", value, err));
    REQUIRE(value == "70");

    REQUIRE(registry.set("towers.macrophage.2.stats.cost", "200", err));
    REQUIRE(toy.cost == 200u);

    REQUIRE(registry.set("towers.macrophage.2.stats.flavor", "sour", err));
    REQUIRE(toy.flavor == Flavor::Sour);

    REQUIRE(registry.set("towers.macrophage.2.stats.drift", "1.5,-2", err));
    REQUIRE(toy.drift.x == 1.5f);
    REQUIRE(toy.drift.y == -2.0f);

    REQUIRE(registry.set("towers.macrophage.2.stats.enabled", "false", err));
    REQUIRE_FALSE(toy.enabled);

    REQUIRE(registry.get("towers.macrophage.2.stats.tint", value, err));
    REQUIRE(value == "0.2,0.8,0.9,0.5");

    REQUIRE(registry.doc("towers.macrophage.2.stats.speed") == "units per second");
}

TEST_CASE("registry rejects bad paths and bad values", "[config][registry]") {
    Toy toy = parse_toy(kValidToy);
    config::Registry registry;
    registry.bind("towers.macrophage.2.stats", kToySchema, &toy);

    std::string value;
    std::string err;

    REQUIRE_FALSE(registry.get("towers.macrophage.2.stats.spee", value, err));
    REQUIRE(contains(err, "did you mean"));

    REQUIRE_FALSE(registry.set("towers.nosuch.0.stats.cost", "1", err));
    REQUIRE(contains(err, "no config field at"));

    REQUIRE_FALSE(registry.set("towers.macrophage.2.stats.flavor", "salty", err));
    REQUIRE(contains(err, "accepted: sweet, sour, bitter"));
    REQUIRE(toy.flavor == Flavor::Bitter);  // unchanged by the failed set

    REQUIRE_FALSE(registry.set("towers.macrophage.2.stats.cost", "banana", err));
    REQUIRE(toy.cost == 70u);
}

TEST_CASE("registry re-binding replaces the target", "[config][registry]") {
    Toy first = parse_toy(kValidToy);
    Toy second = parse_toy(kValidToy);
    second.cost = 999;

    config::Registry registry;
    registry.bind("toy", kToySchema, &first);
    registry.bind("toy", kToySchema, &second);
    REQUIRE(registry.binding_count() == 1);

    std::string value;
    std::string err;
    REQUIRE(registry.get("toy.cost", value, err));
    REQUIRE(value == "999");
}

TEST_CASE("config store loads, hashes and reloads files", "[config][store]") {
    const std::filesystem::path dir = scratch_dir("store");
    const std::string dir_str = dir.string();

    const Toy toy = parse_toy(kValidToy);
    Json document = Json::object();
    document["schema"] = 1;
    config::dump_struct(document, kToySchema, &toy);

    std::string err;
    REQUIRE(config::ConfigStore::write_file(dir_str, "toy.json", document, err));

    config::ConfigStore store;
    store.expect_file("toy.json");
    const config::LoadResult result = store.load_dir(dir_str);
    REQUIRE(result.ok);
    REQUIRE(store.loaded());
    REQUIRE(store.file("toy.json").at("schema").get<int>() == 1);

    const u64 first_hash = store.hash();
    REQUIRE(first_hash != 0);

    // Nothing on disk changed, so a poll must not report a reload: a spurious
    // reload is a visible gameplay hitch.
    std::string poll_err;
    REQUIRE_FALSE(store.poll_changed(poll_err));
    REQUIRE(store.hash() == first_hash);

    document["speed"] = 99.0f;
    REQUIRE(config::ConfigStore::write_file(dir_str, "toy.json", document, err));
    REQUIRE(store.poll_changed(poll_err));
    REQUIRE(store.hash() != first_hash);
    REQUIRE(store.file("toy.json").at("speed").get<f32>() == 99.0f);

    std::filesystem::remove_all(dir);
}

TEST_CASE("config store fails loudly on a missing or malformed file",
          "[config][store]") {
    const std::filesystem::path dir = scratch_dir("bad");
    const std::string dir_str = dir.string();

    config::ConfigStore store;
    store.expect_file("towers.json");
    const config::LoadResult missing = store.load_dir(dir_str);
    REQUIRE_FALSE(missing.ok);
    REQUIRE(contains(missing.error, "towers.json"));
    REQUIRE_FALSE(store.loaded());

    REQUIRE(platform::write_text_file((dir / "towers.json").string(), "{ not json"));
    const config::LoadResult malformed = store.load_dir(dir_str);
    REQUIRE_FALSE(malformed.ok);
    REQUIRE(contains(malformed.error, "parse error"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("config store keeps the last good state when a reload fails",
          "[config][store]") {
    const std::filesystem::path dir = scratch_dir("recover");
    const std::string dir_str = dir.string();

    Json document = Json::object();
    document["speed"] = 1.0f;
    std::string err;
    REQUIRE(config::ConfigStore::write_file(dir_str, "toy.json", document, err));

    config::ConfigStore store;
    store.expect_file("toy.json");
    REQUIRE(store.load_dir(dir_str).ok);

    // A file caught mid-edit is syntactically broken for a few seconds. That
    // must report an error, not retune or crash the running game.
    REQUIRE(platform::write_text_file((dir / "toy.json").string(), "{ \"speed\": "));
    std::string poll_err;
    REQUIRE_FALSE(store.poll_changed(poll_err));
    REQUIRE(contains(poll_err, "parse error"));
    REQUIRE(store.file("toy.json").at("speed").get<f32>() == 1.0f);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// The seven real schemas.
// ---------------------------------------------------------------------------

namespace {

/// Loads a GameConfig from a directory, failing the test with the parser's own
/// message (which names the file and field path) rather than a bare false.
immune::game::GameConfig load_from(const std::string& dir) {
    config::ConfigStore store;
    immune::game::GameConfig cfg;
    std::string err;
    const bool ok = immune::game::load_game_config(store, dir, cfg, err);
    INFO(err);
    REQUIRE(ok);
    return cfg;
}

std::string dump_all(const immune::game::GameConfig& cfg) {
    std::string joined;
    for (const Json& doc : immune::game::dump_game_config(cfg)) joined += doc.dump(2);
    return joined;
}

} // namespace

TEST_CASE("the shipped config files all load", "[config][game]") {
    // Relies on ctest's WORKING_DIRECTORY being the repo root, the same way
    // test_level_loader.cpp reads assets/levels/capillary_test.json.
    const immune::game::GameConfig cfg = load_from("assets/config");

    REQUIRE(cfg.enemies.elites.size() == 5);
    REQUIRE(cfg.waves.find_region("flat") != nullptr);
    REQUIRE(cfg.waves.find_region("organ_chamber") != nullptr);
    // An unknown region must fall back rather than return null: every level
    // string that is not one of the five known regions lands on "flat".
    REQUIRE(cfg.waves.find_region("nonesuch") == cfg.waves.find_region("flat"));
}

TEST_CASE("the shipped config equals the compiled-in tuning", "[config][game]") {
    // The migration guard. While the systems still read their hardcoded
    // tables, this proves assets/config describes exactly those numbers, so
    // switching a system over to the file cannot silently retune the game.
    const immune::game::GameConfig shipped = load_from("assets/config");
    const immune::game::GameConfig compiled = immune::game::default_game_config();
    REQUIRE(dump_all(shipped) == dump_all(compiled));
}

TEST_CASE("game config dump round-trips byte-for-byte", "[config][game]") {
    const std::filesystem::path dir = scratch_dir("game");
    const immune::game::GameConfig original = immune::game::default_game_config();

    std::string err;
    INFO(err);
    REQUIRE(immune::game::write_game_config(original, dir.string(), err));

    const immune::game::GameConfig reloaded = load_from(dir.string());
    REQUIRE(dump_all(reloaded) == dump_all(original));

    std::filesystem::remove_all(dir);
}

TEST_CASE("a missing field in a real config names its file and path",
          "[config][game]") {
    const std::filesystem::path dir = scratch_dir("game_missing");
    std::string err;
    REQUIRE(immune::game::write_game_config(immune::game::default_game_config(), dir.string(), err));

    // Drop one field out of one tier row.
    const std::string path = (dir / "towers.json").string();
    Json doc = Json::parse(*platform::read_text_file(path));
    doc["towers"]["macrophage"]["tiers"][2]["stats"].erase("damage");
    REQUIRE(platform::write_text_file(path, doc.dump(2)));

    config::ConfigStore store;
    immune::game::GameConfig cfg;
    REQUIRE_FALSE(immune::game::load_game_config(store, dir.string(), cfg, err));
    REQUIRE(contains(err, "towers.json"));
    REQUIRE(contains(err, "macrophage"));
    REQUIRE(contains(err, "missing required field 'damage'"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("a tower row cannot carry another role's mechanics", "[config][game]") {
    const std::filesystem::path dir = scratch_dir("game_role");
    std::string err;
    REQUIRE(immune::game::write_game_config(immune::game::default_game_config(), dir.string(), err));

    const std::string path = (dir / "towers.json").string();
    Json doc = Json::parse(*platform::read_text_file(path));
    // A mortar knob pasted into the hydro row: the kind of edit that would
    // otherwise sit in the file doing nothing.
    doc["towers"]["goblet_cell"]["tiers"][0]["mechanics"]["burst_radius"] = 9.0;
    REQUIRE(platform::write_text_file(path, doc.dump(2)));

    config::ConfigStore store;
    immune::game::GameConfig cfg;
    REQUIRE_FALSE(immune::game::load_game_config(store, dir.string(), cfg, err));
    REQUIRE(contains(err, "unknown field 'burst_radius'"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("every config field is addressable from the registry", "[config][game]") {
    immune::game::GameConfig cfg = immune::game::default_game_config();
    config::Registry registry;
    immune::game::bind_game_config(registry, cfg);

    std::string value;
    std::string err;

    REQUIRE(registry.get("towers.macrophage.3.stats.damage", value, err));
    REQUIRE(value == "175");
    REQUIRE(registry.set("towers.macrophage.3.stats.damage", "200", err));
    REQUIRE(cfg.towers.stats[static_cast<u32>(TowerType::Macrophage)][2].damage == 200.0f);

    REQUIRE(registry.get("enemies.families.virus.visual.silhouette", value, err));
    REQUIRE(value == "1.53");
    REQUIRE(registry.set("enemies.families.virus.chaff.max_speed", "1", err) == false);

    REQUIRE(registry.set("enemies.elites.tumor_mass.stats.max_health", "9000", err));
    REQUIRE(registry.get("enemies.elites.tumor_mass.stats.max_health", value, err));
    REQUIRE(value == "9000");

    REQUIRE(registry.set("waves.regions.skin.scaling.count_base", "80", err));
    REQUIRE(registry.set("sim.capacities.max_chaff", "32768", err));
    REQUIRE(registry.set("economy.starting_atp", "500", err));
    REQUIRE(cfg.economy.starting_atp == 500u);
    REQUIRE(registry.set("abilities.fever_response.cooldown_seconds", "30", err));
    REQUIRE(registry.set("meta.win_bonus", "80", err));

    // Every bound field must be readable; a path that parses but does not
    // resolve would be a silent hole in the console's coverage.
    for (const std::string& path : registry.field_paths()) {
        INFO(path);
        REQUIRE(registry.get(path, value, err));
    }
}
