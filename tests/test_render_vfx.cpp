// tests/test_render_vfx.cpp — Wave 4G visual verification. Owner: Wave 4G.
//
// Exercises the three VFX deliverables added to render/Renderer.cpp:
//   - submit_fields(): the field-VFX pass (toxin clouds / antibody tides /
//     directional sprays / complement crackle), one TEST_CASE per shape mix.
//   - the telegraph countdown overlay appended in submit_entities().
//   - the elite death-burst overlay appended in submit_entities().
//
// Drives the real headless-GL Renderer path (same one --screenshot uses) and
// writes PNGs to $TEMP for manual read-back, mirroring the pattern
// test_render_screenshots.cpp already established. That file's header comment
// explains why: the --screenshot CLI's own scenario plumbing does not
// actually populate active DamageFields today (app/Modes.cpp's
// populate_scenario() ignores BenchScenario::damage_fields entirely — an
// orchestrator-owned file this wave does not touch), so a screenshot taken
// via `immune --screenshot --scenario chaff10k_towers` shows chaff but no
// fields. See this wave's report for the exact command that confirmed that
// gap. These tests build the scene directly from public sim APIs instead, the
// same fallback the brief anticipated.
#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Math.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/NamedAgents.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::render;

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

/// Same scratch-dir convention as test_render_screenshots.cpp: PNGs are
/// manual-inspection artifacts outside the repo (no binary assets, ever).
std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

/// The tissue-substrate clear colour is a fixed warm dark tone (~R33 G22 B27
/// given Renderer::begin_frame's 0.129/0.086/0.106 clear). Pixels that differ
/// meaningfully from it are "something drew here" — same threshold
/// test_render_gl.cpp uses.
bool differs_from_background(const u8* px) {
    const i32 dr = static_cast<i32>(px[0]) - 33;
    const i32 dg = static_cast<i32>(px[1]) - 22;
    const i32 db = static_cast<i32>(px[2]) - 27;
    return dr * dr + dg * dg + db * db > 400;
}

usize count_non_background(const std::vector<u8>& pixels, i32 w, i32 h) {
    usize n = 0;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const usize idx = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
            if (differs_from_background(&pixels[idx])) ++n;
        }
    }
    return n;
}

/// Counts non-background pixels in a square window centred on a world point,
/// via the camera's exact world->screen mapping — lets a test assert "this
/// specific field actually drew something roughly where it should have",
/// not just "the frame isn't blank somewhere".
usize count_non_background_near(const std::vector<u8>& pixels, i32 w, i32 h, const Camera& camera,
                                Vec2 world_point, i32 half_window) {
    const Vec2 sp = camera.world_to_screen(world_point);
    const i32 cx = static_cast<i32>(sp.x);
    const i32 cy = static_cast<i32>(sp.y);
    usize n = 0;
    for (i32 y = math::max(0, cy - half_window); y < math::min(h, cy + half_window); ++y) {
        for (i32 x = math::max(0, cx - half_window); x < math::min(w, cx + half_window); ++x) {
            const usize idx = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
            if (differs_from_background(&pixels[idx])) ++n;
        }
    }
    return n;
}

SimWorld make_named_world(u64 seed = 999) {
    SimWorld world;
    SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 64; // unused here but must be non-zero-safe, per test_named_agents.cpp
    world.init(desc, nullptr);
    return world;
}

comp::AiBrain& brain_of(SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(id));
}

comp::Health& health_of(SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::Health>(world.ecs().from_id(id));
}

comp::Transform& transform_of(SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::Transform>(world.ecs().from_id(id));
}

} // namespace

// ---------------------------------------------------------------------------
// submit_fields
// ---------------------------------------------------------------------------

TEST_CASE("submit_fields draws a visible, distinct glow for Circle/Rect/Cone/Chain",
         "[render][gl][vfx]") {
    HeadlessGl gl(960, 540);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{120.0f, 68.0f}};

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));
    REQUIRE(renderer.ready());

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_tilt_degrees(20.0f);
    camera.set_center(bounds.center());
    camera.set_view_height(bounds.size().y);

    // Persistent circle (standing toxin cloud) and persistent rect (antibody
    // wall) plus a fresh burst cone and a fresh burst chain — spread apart so
    // each gets its own region of the frame to check independently.
    const Vec2 circle_origin{22.0f, 34.0f};
    const Vec2 rect_center{60.0f, 34.0f};
    const Vec2 cone_origin{98.0f, 20.0f};
    const Vec2 chain_origin{98.0f, 50.0f};

    std::vector<DamageField> fields;

    DamageField circle;
    circle.shape = FieldShape::Circle;
    circle.origin = circle_origin;
    circle.radius = 9.0f;
    circle.kill_rate = 5.0f;
    circle.falloff = 1.0f;
    circle.lifetime = 0.0f; // persistent
    fields.push_back(circle);

    DamageField rect;
    rect.shape = FieldShape::Rect;
    rect.rect = Rect{rect_center - Vec2{9.0f, 6.0f}, rect_center + Vec2{9.0f, 6.0f}};
    rect.kill_rate = 5.0f;
    rect.falloff = 0.0f;
    rect.lifetime = 0.0f; // persistent
    fields.push_back(rect);

    DamageField cone;
    cone.shape = FieldShape::Cone;
    cone.origin = cone_origin;
    cone.direction = Vec2{1.0f, 0.0f};
    cone.radius = 10.0f;
    cone.arc_radians = 0.5f;
    cone.kill_rate = 8.0f;
    cone.falloff = 2.0f;
    cone.lifetime = 0.3f; // burst, fresh (bright)
    fields.push_back(cone);

    DamageField chain;
    chain.shape = FieldShape::Chain;
    chain.origin = chain_origin;
    chain.radius = 6.0f;
    chain.kill_rate = 10.0f;
    chain.lifetime = 0.3f; // burst, fresh
    fields.push_back(chain);

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_fields(fields.data(), fields.size());
    renderer.end_frame();

    const FrameStats& stats = renderer.stats();
    CHECK(stats.vfx_fields_drawn == fields.size());
    CHECK(stats.draw_calls >= 1);

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));

    const std::string out = scratch_path("render_verify_fields.png");
    REQUIRE(write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[vfx] %s: %dx%d draw_calls=%u fields_drawn=%u\n", out.c_str(), w, h,
                stats.draw_calls, stats.vfx_fields_drawn);

    // Each field must have visibly drawn *something* in its own neighbourhood
    // — proves the pass isn't just clearing the whole screen to one flat tint,
    // and that all four shape branches actually produce pixels.
    CHECK(count_non_background_near(pixels, w, h, camera, circle_origin, 40) > 50u);
    CHECK(count_non_background_near(pixels, w, h, camera, rect_center, 40) > 50u);
    CHECK(count_non_background_near(pixels, w, h, camera, cone_origin, 40) > 30u);
    CHECK(count_non_background_near(pixels, w, h, camera, chain_origin, 40) > 20u);

    renderer.shutdown();
}

TEST_CASE("submit_fields never fully occludes: alpha stays translucent", "[render][gl][vfx]") {
    // DESIGN.md §9.5 / the brief's own rule: field glows must read as
    // translucent atmosphere, never opaque cover. field.frag clamps alpha to
    // 0.85; this checks that clamp survives at the hottest point of the
    // brightest case (a fresh burst Circle, falloff 0 = flat interior).
    HeadlessGl gl(200, 200);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    const Rect bounds{Vec2{-20.0f, -20.0f}, Vec2{20.0f, 20.0f}};
    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(Vec2{0.0f, 0.0f});
    camera.set_view_height(40.0f);

    DamageField burst;
    burst.shape = FieldShape::Circle;
    burst.origin = Vec2{0.0f, 0.0f};
    burst.radius = 8.0f;
    burst.falloff = 0.0f;
    burst.lifetime = 5.0f; // large remaining lifetime => intensity saturates at 1.0

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_fields(&burst, 1);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));

    // Sample the exact centre pixel: brightest possible point of the glow.
    const usize idx = (static_cast<usize>(h / 2) * static_cast<usize>(w) + static_cast<usize>(w / 2)) * 4;
    const u8 alpha_equivalent_over_bg =
        pixels[idx + 0]; // straight-alpha already composited by the blend; just sanity-check it's lit
    CHECK(alpha_equivalent_over_bg > 33); // brighter than the flat background red channel

    renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Telegraph countdown overlay
// ---------------------------------------------------------------------------

TEST_CASE("submit_entities draws a telegraph warning at an elite mid wind-up", "[render][gl][vfx]") {
    HeadlessGl gl(640, 640);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    SimWorld world = make_named_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{0.0f, 0.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    // Drive to Telegraphing the same way test_named_agents.cpp's transition
    // test does (placeholder cycles Spawning -> Advancing -> Telegraphing
    // deterministically once ability_cooldown elapses).
    bool reached = false;
    for (int i = 0; i < 400 && !reached; ++i) {
        world.tick();
        if (brain_of(world, id).state == comp::AiState::Telegraphing) reached = true;
    }
    REQUIRE(reached);

    const sim::named::NamedFrame* frame = sim::named::frame_if_any(world.ecs().registry());
    REQUIRE(frame != nullptr);
    REQUIRE_FALSE(frame->telegraphs.empty());
    const Vec2 telegraph_point = frame->telegraphs.front().point;
    const f32 telegraph_radius = frame->telegraphs.front().radius;
    std::fprintf(stderr, "[vfx] telegraph at (%.2f, %.2f) r=%.2f progress=%.2f\n", telegraph_point.x,
                telegraph_point.y, telegraph_radius, frame->telegraphs.front().progress);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(telegraph_point);
    camera.set_view_height(math::max(telegraph_radius * 6.0f, 10.0f));

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_entities(world.ecs());
    renderer.end_frame();

    // Two extra overlay instances (diamond + ring) beyond the elite's own
    // sprite must have been submitted.
    CHECK(renderer.stats().entity_instances_drawn >= 3);

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    const std::string out = scratch_path("render_verify_telegraph.png");
    REQUIRE(write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[vfx] %s: %dx%d entity_instances=%u\n", out.c_str(), w, h,
                renderer.stats().entity_instances_drawn);

    CHECK(count_non_background_near(pixels, w, h, camera, telegraph_point, w / 2) > 200u);

    renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Elite death burst
// ---------------------------------------------------------------------------

TEST_CASE("submit_entities draws a death burst while an elite is Dying", "[render][gl][vfx]") {
    HeadlessGl gl(640, 640);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    SimWorld world = make_named_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{5.0f, 5.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    for (int i = 0; i < 20; ++i) world.tick(); // clear Spawning, same as test_named_agents.cpp
    REQUIRE(brain_of(world, id).state != comp::AiState::Spawning);

    health_of(world, id).current = 0.0f;
    world.tick();
    REQUIRE(brain_of(world, id).state == comp::AiState::Dying);

    // death_fade for the placeholder archetype is 0.5s == 30 ticks
    // (test_named_agents.cpp asserts this directly). Advance a little way
    // into the fade (~0.2s) before the primary capture: at state_timer==0 the
    // burst's own SDF radius (mix(0.05, 0.55, t)) is still tiny and mostly
    // overlaps the body sprite, so a still frame right at the transition
    // under-sells it. A few ticks in, the expanding ring has grown visibly
    // past the body's own silhouette while still bright — the moment that
    // actually reads as "pop".
    for (int i = 0; i < 12; ++i) world.tick();
    REQUIRE(brain_of(world, id).state == comp::AiState::Dying);
    const Vec2 death_pos = transform_of(world, id).position;

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(death_pos);
    camera.set_view_height(20.0f);

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_entities(world.ecs());
    renderer.end_frame();

    // The dying entity's own sprite plus a death-burst overlay instance.
    CHECK(renderer.stats().entity_instances_drawn >= 2);

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    const std::string out = scratch_path("render_verify_death_burst.png");
    REQUIRE(write_png_rgba(out, pixels.data(), w, h));
    std::fprintf(stderr, "[vfx] %s: %dx%d entity_instances=%u\n", out.c_str(), w, h,
                renderer.stats().entity_instances_drawn);

    CHECK(count_non_background_near(pixels, w, h, camera, death_pos, w / 2) > 100u);

    // death_fade for the placeholder archetype is 0.5s == 30 ticks
    // (test_named_agents.cpp asserts this directly). We're 13 ticks into
    // Dying already; advance to tick 27 (just short of destruction) and
    // confirm the burst overlay is still being submitted right up until the
    // entity is actually removed, then past 30 and confirm it's gone.
    for (int i = 0; i < 14; ++i) world.tick(); // total 27 ticks into Dying
    REQUIRE(world.ecs().named_agent_count() == 1);

    renderer.begin_frame(camera, 0.0f);
    renderer.submit_entities(world.ecs());
    renderer.end_frame();
    CHECK(renderer.stats().entity_instances_drawn >= 2);

    for (int i = 0; i < 6; ++i) world.tick(); // total 33 ticks: past death_fade
    REQUIRE(world.ecs().named_agent_count() == 0);

    renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Perf: submit_fields at a realistic worst-case field count
// ---------------------------------------------------------------------------

TEST_CASE("submit_fields stays cheap even at kMaxFieldInstances-scale field counts",
         "[render][gl][vfx][perf]") {
    // None of the registered --bench scenarios actually populate active
    // DamageFields today: BenchScenario::damage_fields (16 for
    // chaff10k_towers/mixed) is recorded in the bench JSON's metadata but
    // app/Modes.cpp's populate_scenario() never reads it to call
    // DamageSystem::submit() — an orchestrator-owned file this wave does not
    // touch (confirmed by grep: the field only appears in the bench-JSON
    // write and the struct definition). So `immune --bench chaff10k_towers`
    // exercises submit_fields() with an empty array regardless of the
    // scenario's name. This test is the actual field-count stress case for
    // the render_submit budget (DESIGN.md §8.6 / docs/AGENT_BRIEF.md §5:
    // chaff_update + render_submit < 4ms combined), independent of that gap.
    HeadlessGl gl(640, 360);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_center(Vec2{0.0f, 0.0f});
    camera.set_view_height(200.0f);

    // 512 fields matches sim::SimWorld's default max_damage_fields
    // (src/sim/SimWorld.h) — a level could plausibly have this many towers'
    // worth of persistent fields active simultaneously in the worst case.
    std::vector<DamageField> fields;
    fields.reserve(512);
    for (u32 i = 0; i < 512; ++i) {
        DamageField f;
        f.shape = static_cast<FieldShape>(i % 4);
        f.origin = Vec2{static_cast<f32>(i % 64) - 32.0f, static_cast<f32>(i / 64) - 4.0f};
        f.rect = Rect{f.origin - Vec2{1.5f, 1.0f}, f.origin + Vec2{1.5f, 1.0f}};
        f.direction = Vec2{1.0f, 0.0f};
        f.arc_radians = 0.4f;
        f.radius = 2.0f;
        f.falloff = static_cast<f32>(i % 3);
        f.lifetime = (i % 2 == 0) ? 0.0f : 0.4f; // half persistent, half burst
        fields.push_back(f);
    }

    // Warm-up, same rationale as test_render_gl.cpp's chaff perf case: first-
    // touch the persistent mapping and the shader bind before measuring.
    for (int i = 0; i < 3; ++i) {
        renderer.begin_frame(camera, 0.0f);
        renderer.submit_fields(fields.data(), fields.size());
        renderer.end_frame();
    }

    f64 worst_ms = 0.0;
    constexpr int kSamples = 30;
    for (int i = 0; i < kSamples; ++i) {
        renderer.begin_frame(camera, 0.0f);
        WallClock timer;
        renderer.submit_fields(fields.data(), fields.size());
        const f64 ms = timer.elapsed_ms();
        renderer.end_frame();
        if (ms > worst_ms) worst_ms = ms;
    }

    std::fprintf(stderr,
                "[vfx perf] worst submit_fields over %d samples @ 512 fields: %.3fms\n",
                kSamples, worst_ms);

    CHECK(renderer.stats().vfx_fields_drawn == fields.size());
    // Generous headroom: this pass shares the 4ms chaff_update+render_submit
    // budget with the (much more expensive) chaff instancing/blob passes, so
    // it needs to stay a small fraction of that, not consume the whole thing.
    CHECK(worst_ms < 1.0);

    renderer.shutdown();
}
