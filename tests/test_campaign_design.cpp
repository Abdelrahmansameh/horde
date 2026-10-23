#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using namespace immune;
using namespace immune::game;

namespace {
std::vector<LevelDef> campaign_levels() {
    std::vector<std::filesystem::path> files;
    for (const auto& path : platform::list_files(platform::asset_path("levels"), ".json")) {
        if (std::filesystem::path(path).filename().string().starts_with("campaign_")) files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    std::vector<LevelDef> levels;
    for (const auto& file : files) {
        LevelDef level;
        INFO(file.string());
        REQUIRE(LevelLoader{}.load_file(file.string(), level).ok);
        levels.push_back(std::move(level));
    }
    REQUIRE(levels.size() == 10);
    return levels;
}

bool outside(const Rect& frame, Vec2 p, Vec2 half) {
    return p.x + half.x < frame.min.x || p.x - half.x > frame.max.x ||
           p.y + half.y < frame.min.y || p.y - half.y > frame.max.y;
}

std::array<f64, kFamilyCount> campaign_densities() {
    const auto text = platform::read_text_file(platform::asset_path("config/enemies.json"));
    REQUIRE(text.has_value());
    const auto families = nlohmann::json::parse(*text).at("families");
    std::array<f64, kFamilyCount> result{};
    result[static_cast<usize>(PathogenFamily::Virus)] = families.at("virus").at("behavior").at("base_density").get<f64>();
    result[static_cast<usize>(PathogenFamily::Bacteria)] = families.at("bacteria").at("behavior").at("base_density").get<f64>();
    result[static_cast<usize>(PathogenFamily::Parasite)] = families.at("parasite").at("behavior").at("base_density").get<f64>();
    return result;
}

f64 release_density_peak(const WaveDef& wave, const std::array<f64, kFamilyCount>& densities) {
    std::map<f64, f64> changes;
    for (const auto& cohort : wave.spawns) {
        REQUIRE(cohort.duration > 0);
        const f64 rate = cohort.count * densities[static_cast<usize>(cohort.family)] / cohort.duration;
        const f64 start = cohort.start_time;
        changes[start] += rate;
        changes[start + cohort.duration] -= rate;
    }
    f64 rate = 0, peak = 0;
    for (const auto& [time, change] : changes) {
        (void)time;
        rate += change;
        peak = std::max(peak, rate);
    }
    return peak;
}
}

TEST_CASE("campaign ports release distinct bursts and use every entrance and authored route",
          "[level][content][campaign]") {
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        REQUIRE_FALSE(level.waves.empty());
        std::set<std::string> used_ports, used_paths;
        for (const auto& wave : level.waves) {
            INFO(wave.name);
            REQUIRE_FALSE(wave.spawns.empty());
            for (usize i = 0; i < wave.spawns.size(); ++i) {
                const auto& entry = wave.spawns[i];
                REQUIRE(entry.count > 0);
                REQUIRE_FALSE(entry.spawn_point_id.empty());
                used_ports.insert(entry.spawn_point_id);
                const auto port = std::find_if(level.spawn_points.begin(), level.spawn_points.end(),
                    [&](const auto& p) { return p.id == entry.spawn_point_id; });
                REQUIRE(port != level.spawn_points.end());
                for (const auto& id : entry.squad_paths) {
                    const auto path = std::find_if(level.squad_paths.begin(), level.squad_paths.end(),
                        [&](const auto& p) { return p.id == id; });
                    REQUIRE(path != level.squad_paths.end());
                    REQUIRE(path->lane_id == port->lane_id);
                    used_paths.insert(id);
                }
                // A fresh silhouette gets at least 1.25 seconds without another
                // release from this port, including repeats of the same type.
                for (usize j = i + 1; j < wave.spawns.size(); ++j) {
                    const auto& other = wave.spawns[j];
                    if (other.spawn_point_id != entry.spawn_point_id) continue;
                    const f32 end = entry.start_time + entry.duration;
                    const f32 other_end = other.start_time + other.duration;
                    REQUIRE((end + 1.25f <= other.start_time + 0.001f ||
                             other_end + 1.25f <= entry.start_time + 0.001f));
                }
            }
        }
        for (const auto& port : level.spawn_points) REQUIRE(used_ports.contains(port.id));
        for (const auto& path : level.squad_paths) REQUIRE(used_paths.contains(path.id));
    }
}

TEST_CASE("campaign release pressure rises through waves and successive levels",
          "[level][content][campaign][progression]") {
    const auto densities = campaign_densities();
    f64 previous_level = 0;
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        f64 previous_wave = 0;
        for (const auto& wave : level.waves) {
            INFO(wave.name);
            const f64 peak = release_density_peak(wave, densities);
            REQUIRE(peak > previous_wave);
            previous_wave = peak;
        }
        if (previous_level > 0) {
            // The authored 1.32x hypothesis allows geometry-specific anchors;
            // inspect the machine-readable curve before changing this range.
            REQUIRE(previous_wave / previous_level >= 1.30);
            REQUIRE(previous_wave / previous_level <= 1.35);
        }
        previous_level = previous_wave;
    }
}

TEST_CASE("campaign entrances and organ footprints stay outside normal and ultrawide framing",
          "[level][content][campaign][camera]") {
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        for (const i32 width : {1600, 2100}) {
            INFO(width);
            render::Camera camera;
            camera.set_viewport(width, 900);
            camera.set_bounds(level.world_bounds);
            camera.set_center(level.camera.has_center ? level.camera.center : level.world_bounds.center());
            camera.set_view_height(level.camera.view_height > 0 ? level.camera.view_height : level.world_bounds.size().y);
            camera.clamp_to_bounds();
            const Rect frame = camera.visible_bounds();
            for (const auto& port : level.spawn_points) {
                INFO(port.id);
                REQUIRE(outside(frame, port.position, Vec2{port.radius, port.radius}));
            }
            for (const auto& organ : level.objectives) {
                const f32 c = std::abs(std::cos(organ.rotation));
                const f32 s = std::abs(std::sin(organ.rotation));
                const Vec2 half{c * organ.half_extents.x + s * organ.half_extents.y,
                                s * organ.half_extents.x + c * organ.half_extents.y};
                REQUIRE(outside(frame, organ.position, half));
            }
        }
    }
}

TEST_CASE("campaign routes cross walkable tissue and connect their entrance to an offscreen organ",
          "[level][content][campaign][routes]") {
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        sim::SimWorld world;
        sim::SimDesc desc;
        desc.world_bounds = level.world_bounds;
        desc.sim_bounds = level_sim_bounds(level);
        world.init(desc, nullptr);
        LevelLoader loader;
        const auto result = loader.instantiate(level, world);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE_FALSE(level.squad_paths.empty());
        for (const auto& route : level.squad_paths) {
            INFO(route.id);
            REQUIRE(route.points.size() >= 2);
            REQUIRE(std::any_of(level.spawn_points.begin(), level.spawn_points.end(), [&](const auto& port) {
                return port.lane_id == route.lane_id && glm::length(route.points.front() - port.position) <= port.radius;
            }));
            REQUIRE(std::any_of(level.objectives.begin(), level.objectives.end(), [&](const auto& organ) {
                return organ.contains(route.points.back());
            }));
            for (usize i = 1; i < route.points.size(); ++i) {
                const Vec2 delta = route.points[i] - route.points[i - 1];
                const u32 steps = static_cast<u32>(std::ceil(glm::length(delta) / 2.0f));
                for (u32 k = 0; k <= steps; ++k) {
                    const Vec2 p = route.points[i - 1] + delta * (steps ? static_cast<f32>(k) / steps : 0.0f);
                    const auto cell = world.tissue().world_to_cell(p);
                    INFO(p.x << ", " << p.y);
                    REQUIRE(world.tissue().walkable(cell.x, cell.y));
                    REQUIRE(world.flow().reachable(p));
                }
            }
        }
    }
}

TEST_CASE("each campaign entrance damages the hidden organ when undefended",
          "[level][content][campaign][traversal]") {
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        for (const auto& port : level.spawn_points) {
            INFO(port.id);
            sim::SimWorld world;
            sim::SimDesc desc;
            desc.world_bounds = level.world_bounds;
            desc.sim_bounds = level_sim_bounds(level);
            world.init(desc, nullptr);
            REQUIRE(LevelLoader{}.instantiate(level, world).ok);
            const f32 integrity = world.snapshot().objective_integrity;
            REQUIRE(world.chaff_system().spawn_burst(world.chaff(), PathogenFamily::Bacteria,
                port.position, port.radius, 8, world.rng()) == 8);
            // Bare-sim bacteria move at 6 units/sec rather than the campaign
            // roster speed. Long retained routes need more than 100 seconds.
            for (u32 tick = 0; tick < 18000 && world.snapshot().chaff_leaked_total < 8; ++tick)
                world.tick();
            REQUIRE(world.snapshot().chaff_leaked_total == 8);
            REQUIRE(world.snapshot().objective_integrity < integrity);
        }
    }
}
