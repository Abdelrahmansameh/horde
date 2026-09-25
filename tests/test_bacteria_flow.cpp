#include "config/ConfigStore.h"
#include "core/Math.h"
#include "core/Rng.h"
#include "game/config/GameConfig.h"
#include "game/level/Level.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>

using namespace immune;

TEST_CASE("bacteria keep a downstream heading through campaign obstacles",
          "[sim][movement][bacteria]") {
    config::ConfigStore store;
    game::GameConfig cfg;
    std::string error;
    REQUIRE(game::load_game_config(store, "assets/config", cfg, error));
    game::LevelLoader loader;
    game::LevelDef level;
    REQUIRE(loader.load_file("assets/levels/campaign_07_chevron_sieve.json", level).ok);
    sim::SimDesc desc;
    desc.world_bounds = level.world_bounds;
    desc.sim_bounds = game::level_sim_bounds(level);
    desc.max_chaff = 2048;
    desc.squad_tuning = cfg.sim.squads;
    desc.spatial_cell_size = cfg.sim.globals.spatial_cell_size;
    desc.flow_smoothing_radius = cfg.sim.globals.flow_smoothing_radius;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        const auto& family = cfg.enemies.families[f];
        const auto& speed = cfg.enemies.speed_tiers[static_cast<u32>(family.speed_tier)];
        auto& fp = desc.chaff_tuning.family[f];
        fp.max_speed = speed.max_speed;
        fp.acceleration = speed.acceleration;
        fp.jitter = speed.jitter;
        fp.radius = family.visual.silhouette * family.chaff.radius_from_silhouette;
        fp.separation_radius = fp.radius * family.chaff.separation_radius_mul;
        fp.separation_strength = family.chaff.separation_strength;
        fp.alignment_radius = family.chaff.alignment_radius;
        fp.alignment_strength = family.chaff.alignment_strength;
        fp.pressure_threshold = family.chaff.pressure_threshold;
        fp.pressure_gain = family.chaff.pressure_gain;
        fp.pressure_max = family.chaff.pressure_max;
        fp.crowd_relief = family.chaff.crowd_relief;
        fp.contact_spacing = family.chaff.contact_spacing;
        fp.contact_stiffness = family.chaff.contact_stiffness;
        fp.wall_restitution = family.chaff.wall_restitution;
        fp.wall_splash = family.chaff.wall_splash;
    }
    sim::SimWorld world;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(level, world).ok);
    game::WaveDef wave;
    wave.prep_time = 0.0f;
    for (const auto& point : level.spawn_points) {
        game::SpawnEntry entry;
        entry.family = PathogenFamily::Bacteria;
        entry.count = 150;
        entry.duration = 8.0f;
        entry.spawn_point_id = point.id;
        wave.spawns.push_back(entry);
    }
    game::WaveDirector waves;
    waves.set_waves({wave});
    waves.start(world);
    Rng rng(52);
    u64 samples = 0, backwards = 0, sideways = 0;
    for (u32 t = 0; t < 4500; ++t) {
        waves.tick(world, rng, kFixedDt);
        world.tick(nullptr);
        if (t < 60) continue;
        const auto& b = world.chaff();
        for (usize i = 0; i < b.count(); ++i) {
            const Vec2 p{b.pos_x[i], b.pos_y[i]};
            const Vec2 dir = math::normalize_safe(world.flow().sample(p));
            if (math::length_sq(dir) < 0.5f) continue;
            const f32 forward = b.vel_x[i] * dir.x + b.vel_y[i] * dir.y;
            const f32 lateral = std::fabs(b.vel_y[i] * dir.x - b.vel_x[i] * dir.y);
            backwards += forward < -0.1f;
            sideways += lateral > math::max(forward, 0.1f);
            ++samples;
        }
    }
    REQUIRE(samples > 10000);
    const f64 n = static_cast<f64>(samples);
    const auto result = world.snapshot();
    std::fprintf(stderr, "[bacteria campaign] backwards %.4f, sideways %.4f, reached goal %llu\n",
                 backwards / n, sideways / n, result.chaff_leaked_total);
    CHECK(backwards / n < 0.01);
    CHECK(sideways / n < 0.03);
    CHECK(result.chaff_leaked_total > 100);
}
