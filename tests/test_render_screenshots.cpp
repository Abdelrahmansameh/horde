// tests/test_render_screenshots.cpp — visual verification artifacts. Owner:
// Wave 1C.
//
// docs/AGENT_BRIEF.md requires screenshots read back and described by the
// agent that generated them. The `--screenshot` CLI mode (app/Modes.cpp,
// orchestrator-owned) does not yet spawn any chaff for a level — that wiring
// belongs to the wave director / level loader (Wave 2C/2D), not to this one —
// so a plain `immune --screenshot` today shows an empty tissue substrate.
// Until that lands, this test drives the exact same Renderer/Camera/GL path
// `--screenshot` uses, against a synthetic scene built directly from public
// ChaffBuffers/SpatialHash APIs, and writes the frames as real PNGs next to
// the test binary's working directory for manual inspection.
#include "core/JobSystem.h"
#include "core/Math.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace immune;
using namespace immune::sim;
using namespace immune::render;

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

/// Builds a radially-graded, angularly-sectored chaff field: each of the six
/// 60-degree wedges is one PathogenFamily, and density falls off smoothly
/// from the centre outward. That single scene exercises both required shots
/// at once — a wide view has thousands of agents per family at legible mass,
/// and any radius band within a wedge sweeps through the full LOD crossfade
/// (dense core -> blob, thin edge -> instances) with no seam between them.
void build_radial_scene(ChaffBuffers& chaff, Vec2 center, f32 max_radius, f32 spacing) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<f32> jitter(-0.5f * spacing, 0.5f * spacing);
    std::uniform_real_distribution<f32> unit(0.0f, 1.0f);

    for (f32 x = center.x - max_radius; x <= center.x + max_radius; x += spacing) {
        for (f32 y = center.y - max_radius; y <= center.y + max_radius; y += spacing) {
            const Vec2 d{x - center.x, y - center.y};
            const f32 r = std::sqrt(d.x * d.x + d.y * d.y);
            if (r > max_radius) continue;

            const f32 t = math::saturate(r / max_radius);
            const f32 keep_prob = math::lerp(1.0f, 0.03f, t); // dense core -> sparse rim
            if (unit(rng) > keep_prob) continue;

            const f32 angle = std::atan2(d.y, d.x); // -pi..pi
            const u32 sector = static_cast<u32>(
                math::clamp((angle + math::kPi) / math::kTwoPi * static_cast<f32>(kFamilyCount),
                           0.0f, static_cast<f32>(kFamilyCount) - 0.001f));

            if (chaff.full()) return;
            ChaffSpawnParams p;
            p.family = static_cast<PathogenFamily>(sector);
            p.position = Vec2{x + jitter(rng), y + jitter(rng)};
            p.velocity = Vec2{std::cos(angle + math::kPi * 0.5f), std::sin(angle + math::kPi * 0.5f)} * 2.0f;
            p.density = 1.0f;
            chaff.spawn(p);
        }
    }
}

bool render_scene(Renderer& renderer, const Camera& camera, const ChaffBuffers& chaff,
                  const SpatialHash& hash, const std::string& out_path) {
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_chaff(chaff, hash);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    if (!renderer.read_pixels(pixels, w, h)) return false;

    const FrameStats& stats = renderer.stats();
    std::fprintf(stderr,
                "[screenshot] %s: %dx%d draw_calls=%u instances=%u blob_agents=%u chaff=%zu\n",
                out_path.c_str(), w, h, stats.draw_calls, stats.chaff_instances_drawn,
                stats.chaff_agents_in_blobs, chaff.count());

    return write_png_rgba(out_path, pixels.data(), w, h);
}

/// A writable scratch directory outside the repo: $TEMP on Windows, falling
/// back to the working directory. These PNGs are manual-inspection artifacts,
/// not part of the repo, so they deliberately don't land under assets/.
std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

} // namespace

TEST_CASE("visual verification: wide view of ~10k agents across all families",
         "[render][gl][screenshot]") {
    HeadlessGl gl(1280, 720);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    const Rect bounds{Vec2{-40.0f, -40.0f}, Vec2{100.0f, 100.0f}};
    ChaffBuffers chaff;
    chaff.reserve(20000);
    build_radial_scene(chaff, Vec2{30.0f, 30.0f}, 34.0f, 0.42f);
    REQUIRE(chaff.count() > 5000);

    SpatialHash hash;
    SpatialHashDesc hd; hd.bounds = bounds; hd.cell_size = 3.0f;
    hash.configure(hd);
    JobSystem jobs(0u);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), &jobs);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = 16384;
    // Opt in: the blob pass ships disabled (RendererDesc::lod_blob_enabled).
    // These two cases exist to look at it, so they are where it gets turned on.
    rd.lod_blob_enabled = true;
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_tilt_degrees(20.0f);
    camera.set_center(Vec2{30.0f, 30.0f});
    camera.set_view_height(76.0f);

    const std::string out = scratch_path("render_verify_wide_10k.png");
    REQUIRE(render_scene(renderer, camera, chaff, hash, out));

    const FrameStats& stats = renderer.stats();
    CHECK(stats.chaff_instances_drawn > 0);
    CHECK(stats.chaff_agents_in_blobs > 0); // the core must have crossed into blob territory
    CHECK(stats.draw_calls <= kFamilyCount + 1);

    renderer.shutdown();
}

TEST_CASE("visual verification: close-up crop across the LOD crossfade band",
         "[render][gl][screenshot]") {
    HeadlessGl gl(1000, 1000);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    const Rect bounds{Vec2{-40.0f, -40.0f}, Vec2{100.0f, 100.0f}};
    ChaffBuffers chaff;
    chaff.reserve(20000);
    build_radial_scene(chaff, Vec2{30.0f, 30.0f}, 34.0f, 0.42f);

    SpatialHash hash;
    SpatialHashDesc hd; hd.bounds = bounds; hd.cell_size = 3.0f;
    hash.configure(hd);
    JobSystem jobs(0u);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), &jobs);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = 16384;
    // Opt in: the blob pass ships disabled (RendererDesc::lod_blob_enabled).
    // These two cases exist to look at it, so they are where it gets turned on.
    rd.lod_blob_enabled = true;
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    // Zoom along one wedge's bisector (angle in [0, pi/3)) from the dense
    // core out past the rim, so a single frame
    // sweeps blob -> crossfade band -> pure instances without crossing into
    // a neighbouring family's colour.
    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_tilt_degrees(20.0f);
    const f32 bisector = math::kPi / 6.0f; // 30 degrees, mid-sector-3
    const Vec2 dir{std::cos(bisector), std::sin(bisector)};
    camera.set_center(Vec2{30.0f, 30.0f} + dir * 11.0f);
    camera.set_view_height(24.0f);

    const std::string out = scratch_path("render_verify_lod_crossfade.png");
    REQUIRE(render_scene(renderer, camera, chaff, hash, out));

    const FrameStats& stats = renderer.stats();
    CHECK(stats.chaff_instances_drawn > 0);
    CHECK(stats.chaff_agents_in_blobs > 0);

    renderer.shutdown();
}

TEST_CASE("visual verification: tissue substrate renders a vessel band",
         "[render][gl][screenshot]") {
    // The level loader (Wave 2D) does not yet rasterize splines into a
    // TissueMask or call DistanceField::bake() (grep confirms neither is
    // called anywhere outside tests today), so a real `--screenshot` run
    // currently has a zero-sized DistanceField and submit_tissue correctly
    // no-ops on it. This test builds a mask/SDF directly — the same way
    // tests/test_flowfield_mask.cpp does — to prove the tissue pass itself
    // is correct and ready for that wiring.
    HeadlessGl gl(800, 400);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    TissueMask mask;
    mask.resize(160, 80, 0.5f, Vec2{0.0f, 0.0f}); // 80x40 world units
    for (i32 y = 0; y < 80; ++y) {
        for (i32 x = 0; x < 160; ++x) {
            // A horizontal vessel band through the vertical middle third.
            const bool inside = y > 26 && y < 54;
            mask.set_walkable(x, y, inside);
        }
    }
    DistanceField sdf;
    sdf.bake(mask);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(Vec2{40.0f, 20.0f});
    camera.set_view_height(40.0f);
    camera.set_tilt_degrees(20.0f);

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(mask, sdf, 0.0f);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    const std::string out = scratch_path("render_verify_tissue.png");
    REQUIRE(write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[screenshot] %s: %dx%d\n", out.c_str(), w, h);

    // Sample the vertical centre column: middle rows (inside the vessel)
    // must be visibly brighter than rows near the top/bottom edges (outside).
    const i32 cx = w / 2;
    const auto luma_at = [&](i32 y) {
        const usize idx = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(cx)) * 4;
        return static_cast<i32>(pixels[idx + 0]) + pixels[idx + 1] + pixels[idx + 2];
    };
    const i32 inside_luma = luma_at(h / 2);
    const i32 outside_luma = luma_at(h / 8);
    CHECK(inside_luma > outside_luma + 30);

    renderer.shutdown();
}

TEST_CASE("visual verification: an in-lane obstacle renders as vessel wall, not as a prop",
          "[render][gl][screenshot][obstacles]") {
    // The claim in-lane obstacles are built on (sim/flowfield/ObstacleRaster.h)
    // is that an island carved out of the lumen is the SAME MATERIAL as the
    // lane's outer boundary -- not a decal, not a sprite, not a separate pass.
    // Nothing in the renderer knows obstacles exist; they reach the screen
    // purely as SDF, so the only way to check the claim is to render a level
    // that has one and read the pixels back.
    HeadlessGl gl(900, 600);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    game::LevelLoader loader;
    game::LevelDef def;
    const std::string level = platform::asset_path("levels/plaque_field.json");
    REQUIRE(platform::file_exists(level));
    REQUIRE(loader.load_file(level, def).ok);

    SimWorld world;
    SimDesc sd;
    sd.world_bounds = def.world_bounds;
    world.init(sd, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    // Framed on the "plaque" disc: a 14-unit island at (120,120) sitting in a
    // 76-unit lane. Straight down, so world_to_screen below is a plain scale
    // and the probes land where the arithmetic says they do.
    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(Vec2{120.0f, 121.0f});
    camera.set_view_height(120.0f);
    camera.set_tilt_degrees(0.0f);

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    const std::string out = scratch_path("render_verify_obstacle.png");
    REQUIRE(write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[screenshot] %s: %dx%d\n", out.c_str(), w, h);

    const auto luma_at_world = [&](Vec2 world_pos) {
        const Vec2 s = camera.world_to_screen(world_pos);
        const i32 x = math::clamp(static_cast<i32>(s.x), 0, w - 1);
        const i32 y = math::clamp(static_cast<i32>(s.y), 0, h - 1);
        const usize idx = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
        return static_cast<i32>(pixels[idx + 0]) + pixels[idx + 1] + pixels[idx + 2];
    };

    // Three probes, all at comparable distance from a boundary so the wall
    // shading is compared against wall shading rather than against flat
    // interstitium: a few units inside the island, the open lumen beside it,
    // and a few units past the lane's own outer wall.
    const i32 island = luma_at_world(Vec2{120.0f, 127.0f});
    const i32 lumen = luma_at_world(Vec2{120.0f, 145.0f});
    const i32 outer_wall = luma_at_world(Vec2{120.0f, 166.0f});
    std::fprintf(stderr, "[obstacle] luma island=%d lumen=%d outer_wall=%d\n", island, lumen,
                 outer_wall);

    // It reads as flesh, not as plasma...
    CHECK(lumen > island + 30);
    CHECK(lumen > outer_wall + 30);
    // ...and specifically as the SAME flesh the lane is carved out of: the two
    // wall samples sit far closer to each other than either does to the lumen.
    CHECK(std::abs(island - outer_wall) < (lumen - island) / 2);

    renderer.shutdown();
}
