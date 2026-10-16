#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

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

u32 population(const WaveDef& wave) {
    u32 total = 0;
    for (const auto& spawn : wave.spawns) total += spawn.count;
    return total;
}

bool outside(const Rect& frame, Vec2 p, Vec2 half) {
    return p.x + half.x < frame.min.x || p.x - half.x > frame.max.x ||
           p.y + half.y < frame.min.y || p.y - half.y > frame.max.y;
}
}

TEST_CASE("campaign ports release distinct bursts and use every entrance and authored route",
          "[level][content][campaign]") {
    u32 previous_opening = 0, previous_final = 0, previous_total = 0;
    usize previous_wave_count = 0;
    for (const auto& level : campaign_levels()) {
        INFO(level.name);
        std::set<std::string> used_ports, used_paths;
        u32 total = 0, previous_wave = 0;
        for (const auto& wave : level.waves) {
            INFO(wave.name);
            const u32 count = population(wave);
            REQUIRE(count >= previous_wave);
            previous_wave = count;
            total += count;
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
                // A fresh silhouette gets at least one second without another
                // release from this port, including repeats of the same type.
                for (usize j = i + 1; j < wave.spawns.size(); ++j) {
                    const auto& other = wave.spawns[j];
                    if (other.spawn_point_id != entry.spawn_point_id) continue;
                    const f32 end = entry.start_time + entry.duration;
                    const f32 other_end = other.start_time + other.duration;
                    REQUIRE((end + 1.0f <= other.start_time + 0.001f ||
                             other_end + 1.0f <= entry.start_time + 0.001f));
                }
            }
        }
        for (const auto& port : level.spawn_points) REQUIRE(used_ports.contains(port.id));
        for (const auto& path : level.squad_paths) REQUIRE(used_paths.contains(path.id));
        REQUIRE(population(level.waves.front()) > previous_opening);
        REQUIRE(population(level.waves.back()) > previous_final);
        REQUIRE(total > previous_total);
        REQUIRE(level.waves.size() >= previous_wave_count);
        previous_opening = population(level.waves.front());
        previous_final = population(level.waves.back());
        previous_total = total;
        previous_wave_count = level.waves.size();
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
