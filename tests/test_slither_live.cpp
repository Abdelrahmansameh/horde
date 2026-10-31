// Real navigation traces: steady-circle fixtures cannot expose tick-to-tick
// steering noise amplified into a whole-body bend.
#include "core/Math.h"
#include "core/Rng.h"
#include "game/config/GameConfig.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "game/level/Level.h"
#include "game/session/LevelSession.h"
#include "game/session/RunSetup.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace immune;

namespace {
constexpr u32 kParasite = static_cast<u32>(PathogenFamily::Parasite);

Vec2 warped_point(f32 x, f32 y, f32 curvature) {
    if (std::fabs(curvature) < 1e-4f) return {x, y};
    const f32 a = curvature * x;
    const f32 sn = std::sin(a), cs = std::cos(a);
    const f32 h = std::sin(0.5f * a);
    return {sn / curvature - y * sn, 2.0f * h * h / curvature + y * cs};
}

f32 path_y(f32 x, f32 phase, const sim::SlitherParams& p, f32 scale) {
    const f32 q = phase + math::kTwoPi * scale * x / p.wavelength;
    return p.amplitude * p.head_amplitude * std::sin(q) +
           0.5f * p.curl * std::sin(0.5f * q + 1.3f) +
           p.probe * std::sin(2.0f * q + 0.6f);
}

struct Previous {
    bool valid = false;
    u8 state = 0;
    Vec2 position{};
    f32 curvature = 0.0f;
    f32 heading = 0.0f;
    f32 phase = 0.0f;
};

struct LiveMetrics {
    std::vector<f32> curvature_jumps;
    f32 max_root_step = 0.0f;
    f32 max_turn = 0.0f;
    u32 sign_flips = 0;
    u32 saturated = 0;
    u32 transitions = 0;
    u32 samples = 0;
    f32 interpolation_boundary_jump = 0.0f;
};

Vec2 head_position(Vec2 position, f32 heading, f32 phase, f32 curvature,
                   f32 scale, const sim::SlitherParams& sl) {
    const f32 x = 0.5f * sl.body_length;
    const Vec2 local = warped_point(x, path_y(x, phase, sl, scale), curvature * scale) * scale;
    const f32 sn = std::sin(heading), cs = std::cos(heading);
    return position + Vec2{cs * local.x - sn * local.y, sn * local.x + cs * local.y};
}

f32 percentile(std::vector<f32> values, f32 fraction) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    return values[static_cast<usize>(fraction * static_cast<f32>(values.size() - 1))];
}
} // namespace

TEST_CASE("live parasite navigation measures transient body deformation on real levels",
          "[sim][burrow][slither][slither-live]") {
    config::ConfigStore store;
    game::GameConfig config;
    std::string error;
    REQUIRE(game::load_game_config(store, "assets/config", config, error));
    const game::EnemyConfig saved = game::enemy_config();
    struct Restore {
        game::EnemyConfig saved;
        ~Restore() { game::EnemyRoster roster; game::apply_enemy_config(roster, saved); }
    } restore{saved};
    game::EnemyRoster roster;
    game::apply_enemy_config(roster, config.enemies);

    const char* out_dir = std::getenv("IMMUNE_SLITHER_LIVE_OUT");
    if (out_dir != nullptr) REQUIRE(platform::ensure_directory(out_dir));
    const char* capture_dir = std::getenv("IMMUNE_SLITHER_LIVE_FRAMES");
    std::unique_ptr<platform::Window> window;
    std::unique_ptr<render::Renderer> renderer;
    if (capture_dir != nullptr && capture_dir[0] != '\0') {
        REQUIRE(platform::ensure_directory(capture_dir));
        window = std::make_unique<platform::Window>();
        REQUIRE(platform::create_headless_gl(*window, 960, 540));
        renderer = std::make_unique<render::Renderer>();
        render::RendererDesc rd;
        rd.framebuffer_width = 960;
        rd.framebuffer_height = 540;
        rd.max_chaff_instances = 256;
        REQUIRE(renderer->init(rd));
    }

    struct Scenario { const char* label; const char* file; Vec2 spawn; u32 count; bool authored = false; };
    for (const Scenario scenario : {
             Scenario{"gym_alone", "assets/levels/gym.json", {400.0f, 395.0f}, 1},
             Scenario{"gym_crowd", "assets/levels/gym.json", {400.0f, 395.0f}, 96},
             Scenario{"bend_alone", "assets/levels/campaign_01_first_bend.json", {108.0f, 94.0f}, 1},
             Scenario{"bend_crowd", "assets/levels/campaign_01_first_bend.json", {108.0f, 94.0f}, 96},
             Scenario{"gym_authored", "assets/levels/gym.json", {}, 0, true}}) {
        INFO(scenario.label);
        game::LevelDef level;
        const auto loaded = game::LevelLoader{}.load_file(scenario.file, level);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        auto desc = game::make_level_sim_desc(config, roster, level, 42);
        desc.max_chaff = scenario.authored ? 4096 : 256;
        desc.max_swarmers = 32;
        desc.max_fluid_particles = 32;
        auto world = std::make_unique<sim::SimWorld>();
        world->init(desc, nullptr);
        const auto instantiated = game::LevelLoader{}.instantiate(level, *world);
        INFO(instantiated.error);
        REQUIRE(instantiated.ok);
        Rng rng(42);
        game::WaveDirector waves;
        game::LevelSystems systems;
        systems.world = world.get();
        systems.enemies = &roster;
        if (scenario.authored) {
            // The actual first gym wave: mixed families, authored timing,
            // markers, streaming and squad-path selection remain intact.
            REQUIRE_FALSE(level.waves.empty());
            waves.set_waves({level.waves.front()});
            waves.start(*world);
            waves.request_early_start();
            systems.waves = &waves;
        } else {
            REQUIRE(world->chaff_system().spawn_burst(world->chaff(), PathogenFamily::Parasite,
                                                    scenario.spawn, scenario.count == 1 ? 0.0f : 12.0f,
                                                    scenario.count, rng) == scenario.count);
        }
        std::ofstream csv;
        if (out_dir != nullptr) {
            csv.open(std::string(out_dir) + "/" + scenario.label + ".csv");
            REQUIRE(csv.good());
            csv << "tick,generation,state,x,y,dx,dy,heading,turn,phase,phase_delta,curvature,curvature_head_jump,"
                   "vx,vy,sim_prev_x,sim_prev_y,sdf_prev,sdf_now,wall_radius\n";
        }
        std::vector<Previous> previous(16384);
        LiveMetrics metrics;
        const auto& sl = desc.burrow_tuning.slither[kParasite];
        render::ChaffBatchParams batch_params;
        batch_params.lod_blob_enabled = false;
        batch_params.per_family_capacity = static_cast<u32>(desc.max_chaff);
        const render::OccupancyGrid empty_occupancy{};
        std::vector<render::ChaffInstance> batches[3];
        for (auto& batch : batches) batch.resize(kFamilyCount * desc.max_chaff);
        render::Camera camera;
        camera.set_viewport(960, 540);
        camera.set_center({125.0f, 85.0f});
        camera.set_view_height(60.0f);
        for (u32 tick = 0; tick < 600; ++tick) {
            if (scenario.authored) game::step_level(systems);
            else world->tick();
            const auto& chaff = world->chaff();
            const bool inspect_interpolation = tick % 30 == 0;
            if (inspect_interpolation) {
                for (u32 frame = 0; frame < 3; ++frame) {
                    batch_params.interpolation_alpha = 0.5f * static_cast<f32>(frame);
                    render::build_chaff_batches(chaff, empty_occupancy, batch_params, batches[frame].data(), nullptr);
                }
            }
            u32 parasite_cursor = 0;
            for (usize i = 0; i < chaff.count(); ++i) {
                if (chaff.family[i] != kParasite) continue;
                const bool visible = chaff.burrow_state[i] != sim::burrow_state::kUnderground || chaff.burrow_anim[i] > 0.0f;
                const usize instance_offset = kParasite * desc.max_chaff + parasite_cursor;
                if (visible) ++parasite_cursor;
                const u32 generation = chaff.generation[i];
                REQUIRE(generation < previous.size());
                auto& old = previous[generation];
                const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
                const f32 curvature = chaff.slither_curvature[i];
                f32 jump = 0.0f;
                Vec2 displacement{};
                if (old.valid) {
                    if (old.state != chaff.burrow_state[i]) ++metrics.transitions;
                    displacement = p - old.position;
                    if (old.state == sim::burrow_state::kSurface &&
                        chaff.burrow_state[i] == sim::burrow_state::kSurface) {
                        const f32 scale = render::family_visual(PathogenFamily::Parasite).silhouette * chaff.size_scale[i];
                        const f32 x = 0.5f * sl.body_length;
                        const f32 y = path_y(x, chaff.slither_phase[i], sl, scale);
                        const Vec2 now_shape = warped_point(x, y, curvature * scale);
                        const Vec2 old_shape = warped_point(x, y, old.curvature * scale);
                        jump = math::length(now_shape - old_shape) * scale;
                        // Deformation must stay tied to actual translation,
                        // rather than creating a body-width snap at low speed.
                        const f32 spatial_budget = 0.25f * std::min(math::length(displacement), sl.wavelength / 60.0f);
                        CHECK(jump <= spatial_budget + 0.003f);
                        const auto& family_motion = desc.chaff_tuning.family[kParasite];
                        const f32 nominal_step = family_motion.max_speed / 60.0f;
                        const f32 wall_radius = (family_motion.wall_radius > 0.0f
                            ? family_motion.wall_radius : family_motion.radius) * chaff.size_scale[i];
                        // Ghost-family contact spacing must settle gradually.
                        // Far from walls, a normal movement step plus at most
                        // one quarter step of contact correction is enough;
                        // metre-scale snaps are not a wall recovery here.
                        const f32 old_clearance = world->sdf().sample(old.position);
                        const f32 new_clearance = world->sdf().sample(p);
                        if (old_clearance >= wall_radius + nominal_step &&
                            new_clearance >= wall_radius + nominal_step) {
                            CHECK(math::length(displacement) <= 1.25f * nominal_step + 0.003f);
                        }
                        metrics.curvature_jumps.push_back(jump);
                        metrics.max_root_step = std::max(metrics.max_root_step, math::length(displacement));
                        metrics.max_turn = std::max(metrics.max_turn, std::fabs(chaff.body_heading_delta[i]));
                        if (curvature * old.curvature < 0.0f &&
                            std::fabs(curvature) > 0.005f && std::fabs(old.curvature) > 0.005f) ++metrics.sign_flips;
                        const f32 half = desc.burrow_tuning.body_half_length[kParasite] * chaff.size_scale[i];
                        if (half > 0.0f && std::fabs(curvature) >= 0.99f * 0.6f / half) ++metrics.saturated;
                        ++metrics.samples;
                        if (inspect_interpolation && visible) {
                            const Vec2 previous_head = head_position(old.position, old.heading, old.phase,
                                                                    old.curvature, scale, sl);
                            for (u32 frame = 0; frame < 3; ++frame) {
                                const auto& instance = batches[frame][instance_offset];
                                const Vec2 rendered_head = head_position({instance.x, instance.y}, instance.rotation,
                                    instance.anim_phase, instance.curvature / instance.scale, instance.scale, sl);
                                CHECK(std::isfinite(rendered_head.x));
                                CHECK(std::isfinite(rendered_head.y));
                                if (frame == 0) {
                                    const f32 boundary_jump = math::length(rendered_head - previous_head);
                                    metrics.interpolation_boundary_jump = std::max(metrics.interpolation_boundary_jump, boundary_jump);
                                    // alpha=0 of the next tick must reconstruct
                                    // alpha=1 of the preceding tick, including bend.
                                    CHECK(boundary_jump < 0.001f);
                                }
                                if (frame == 1) {
                                    const f32 rendered_curvature = instance.curvature / instance.scale;
                                    CHECK(rendered_curvature >= std::min(old.curvature, curvature) - 1e-6f);
                                    CHECK(rendered_curvature <= std::max(old.curvature, curvature) + 1e-6f);
                                }
                                if (frame == 2) {
                                    const Vec2 current_head = head_position(p, chaff.body_heading[i], chaff.slither_phase[i],
                                                                           curvature, scale, sl);
                                    CHECK(math::length(rendered_head - current_head) < 0.001f);
                                }
                            }
                        }
                    }
                }
                CHECK(std::isfinite(curvature));
                CHECK(std::isfinite(chaff.slither_phase[i]));
                if (csv.is_open()) csv << tick << ',' << generation << ',' << static_cast<u32>(chaff.burrow_state[i])
                    << ',' << p.x << ',' << p.y << ',' << displacement.x << ',' << displacement.y
                    << ',' << chaff.body_heading[i] << ',' << chaff.body_heading_delta[i]
                    << ',' << chaff.slither_phase[i] << ',' << chaff.slither_phase_delta[i]
                    << ',' << curvature << ',' << jump << ',' << chaff.vel_x[i] << ',' << chaff.vel_y[i]
                    << ',' << chaff.prev_pos_x[i] << ',' << chaff.prev_pos_y[i]
                    << ',' << world->sdf().sample({chaff.prev_pos_x[i], chaff.prev_pos_y[i]})
                    << ',' << world->sdf().sample(p) << ','
                    << (desc.chaff_tuning.family[kParasite].wall_radius > 0.0f
                            ? desc.chaff_tuning.family[kParasite].wall_radius
                            : desc.chaff_tuning.family[kParasite].radius) * chaff.size_scale[i] << '\n';
                old = {true, chaff.burrow_state[i], p, curvature, chaff.body_heading[i], chaff.slither_phase[i]};
            }
            // Capture genuine movement from the live level, including its
            // steering, crowd, burrow and interpolation state.
            if (renderer && std::string(scenario.label) == "bend_crowd" && tick >= 30 && tick < 150) {
                renderer->begin_frame(camera, 0.5f);
                renderer->submit_tissue(world->tissue(), world->sdf(), static_cast<f32>(tick) / 60.0f);
                renderer->submit_chaff(chaff, world->spatial());
                renderer->end_frame();
                std::vector<u8> pixels;
                i32 width = 0, height = 0;
                REQUIRE(renderer->read_pixels(pixels, width, height));
                char name[48];
                std::snprintf(name, sizeof(name), "/frame_%03u.png", tick - 30);
                REQUIRE(render::write_png_rgba(std::string(capture_dir) + name, pixels.data(), width, height));
            }
        }
        REQUIRE(metrics.samples > 100);
        const f32 p95 = percentile(metrics.curvature_jumps, 0.95f);
        const f32 p99 = percentile(metrics.curvature_jumps, 0.99f);
        const f32 maximum = percentile(metrics.curvature_jumps, 1.0f);
        // A pose must not jump several body widths merely because the
        // navigation heading changed slightly between adjacent fixed ticks.
        // These budgets are comfortably above the actual .17-unit travel
        // step's permitted deformation, and fail the former 3-unit snaps.
        CHECK(p99 < 0.05f);
        CHECK(maximum < 0.08f);
        std::fprintf(stderr, "[slither live] %s: samples=%u curvature-only head jump p95=%.6f p99=%.6f max=%.6f "
                             "world/tick, root max=%.6f, turn max=%.6f rad, sign flips=%u saturation=%u transitions=%u interpolation boundary=%.8f\n",
                     scenario.label, metrics.samples, p95, p99, maximum, metrics.max_root_step,
                     metrics.max_turn, metrics.sign_flips, metrics.saturated, metrics.transitions, metrics.interpolation_boundary_jump);
        if (out_dir != nullptr) {
            std::ofstream summary(std::string(out_dir) + "/" + scenario.label + ".json");
            REQUIRE(summary.good());
            summary << "{\"scenario\":\"" << scenario.label << "\",\"samples\":" << metrics.samples
                    << ",\"curvature_head_jump_p95\":" << p95 << ",\"curvature_head_jump_p99\":" << p99
                    << ",\"curvature_head_jump_max\":" << maximum << ",\"root_step_max\":" << metrics.max_root_step
                    << ",\"turn_max\":" << metrics.max_turn << ",\"curvature_sign_flips\":" << metrics.sign_flips
                    << ",\"curvature_saturated\":" << metrics.saturated << ",\"burrow_transitions\":" << metrics.transitions
                    << ",\"interpolation_boundary_jump_max\":" << metrics.interpolation_boundary_jump << "}\n";
        }
    }
    if (renderer) renderer->shutdown();
}
