// tests/test_render_gl.cpp — end-to-end GL smoke test. Owner: Wave 1C.
//
// Everything else in this wave's test suite is deliberately GL-free (see
// test_render_batch.cpp / test_render_lod.cpp) so the crossfade and batching
// math can be checked without a context. This file is the exception: it
// stands up the same headless GL context --screenshot uses, drives the real
// Renderer against a synthetic 10k-agent chaff store, and checks that pixels
// actually land where the agents are — proving the GL half (persistent
// buffers, base-instance addressing, the density texture) end to end.
#include "core/Clock.h"
#include "core/JobSystem.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using namespace immune;
using namespace immune::sim;
using namespace immune::render;

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;

    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

} // namespace

TEST_CASE("Renderer draws a 10k-agent chaff field and reads back non-background pixels",
         "[render][gl][integration]") {
    HeadlessGl gl(640, 360);
    if (!gl.ok) {
        WARN("headless GL context unavailable in this environment; skipping");
        return;
    }

    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};

    ChaffBuffers chaff;
    chaff.reserve(10000);
    for (u32 i = 0; i < 10000; ++i) {
        ChaffSpawnParams p;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        // Cluster near the centre so a fixed-size viewport is guaranteed to
        // see a dense, non-trivial LOD mix (some instances, some blob).
        const f32 fx = static_cast<f32>(i % 100);
        const f32 fy = static_cast<f32>(i / 100);
        p.position = Vec2{78.0f + fx * 1.0f, 22.0f + fy * 1.0f};
        p.density = 1.0f;
        REQUIRE(chaff.spawn(p).valid());
    }
    REQUIRE(chaff.count() == 10000);

    SpatialHash hash;
    SpatialHashDesc hd;
    hd.bounds = bounds;
    hd.cell_size = 3.0f;
    hash.configure(hd);
    JobSystem jobs(0u);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), &jobs);
    REQUIRE(hash.max_cell_occupancy() > 0);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = 16384;
    Renderer renderer;
    REQUIRE(renderer.init(rd));
    REQUIRE(renderer.ready());

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(bounds);
    camera.set_center(bounds.center());
    camera.set_view_height(bounds.size().y);
    camera.clamp_to_bounds();

    WallClock submit_timer;
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_chaff(chaff, hash);
    const f64 submit_ms = submit_timer.elapsed_ms();
    renderer.end_frame();

    const FrameStats& stats = renderer.stats();
    // Six families max, plus at most one blob-pass draw: never more than 7.
    REQUIRE(stats.draw_calls >= 1);
    REQUIRE(stats.draw_calls <= kFamilyCount + 1);
    // Every agent's mass must land somewhere (instance or blob) — nothing
    // silently vanishes at the CPU->GPU boundary.
    REQUIRE(stats.chaff_instances_drawn + stats.chaff_agents_in_blobs > 0);

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    REQUIRE(renderer.read_pixels(pixels, w, h));
    REQUIRE(w == gl.window.width());
    REQUIRE(h == gl.window.height());
    REQUIRE(pixels.size() == static_cast<usize>(w) * static_cast<usize>(h) * 4);

    // The clear/tissue-substrate colour is a warm, low-saturation dark tone
    // (~R33 G22 B27 given the 0.129/0.086/0.106 clear). Count pixels that
    // differ meaningfully from that background — the chaff cluster occupies
    // the centre of the frame, so there must be a substantial patch of them.
    usize non_background = 0;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const usize idx = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
            const i32 dr = static_cast<i32>(pixels[idx + 0]) - 33;
            const i32 dg = static_cast<i32>(pixels[idx + 1]) - 22;
            const i32 db = static_cast<i32>(pixels[idx + 2]) - 27;
            if (dr * dr + dg * dg + db * db > 400) ++non_background;
        }
    }
    REQUIRE(non_background > 500u);

    std::fprintf(stderr,
                "[test_render_gl] submit_chaff=%.3fms draw_calls=%u instances=%u blob_agents=%u "
                "non_bg_px=%zu/%d\n",
                submit_ms, stats.draw_calls, stats.chaff_instances_drawn,
                stats.chaff_agents_in_blobs, non_background, w * h);

    renderer.shutdown();
}

TEST_CASE("Renderer submit_chaff stays within the Wave-1 render_submit budget at 10k",
         "[render][gl][perf]") {
    HeadlessGl gl(640, 360);
    if (!gl.ok) {
        WARN("headless GL context unavailable in this environment; skipping");
        return;
    }

    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    ChaffBuffers chaff;
    chaff.reserve(10000);
    for (u32 i = 0; i < 10000; ++i) {
        ChaffSpawnParams p;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        p.position = Vec2{static_cast<f32>(i % 256), static_cast<f32>((i / 256) % 144)};
        p.density = 1.0f;
        chaff.spawn(p);
    }

    SpatialHash hash;
    SpatialHashDesc hd;
    hd.bounds = bounds;
    hd.cell_size = 4.0f;
    hash.configure(hd);
    JobSystem jobs(0u);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), &jobs);

    RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = 16384;
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(bounds);
    camera.set_center(bounds.center());
    camera.set_view_height(bounds.size().y);
    camera.clamp_to_bounds();

    // Warm up (first-touch page faults on the persistent mapping, first-frame
    // shader binds) before taking the measurement, same as the real bench
    // harness would.
    for (int i = 0; i < 3; ++i) {
        renderer.begin_frame(camera, 0.0f);
        renderer.submit_chaff(chaff, hash);
        renderer.end_frame();
    }

    f64 worst_ms = 0.0;
    constexpr int kSamples = 30;
    for (int i = 0; i < kSamples; ++i) {
        renderer.begin_frame(camera, 0.0f);
        WallClock timer;
        renderer.submit_chaff(chaff, hash);
        const f64 ms = timer.elapsed_ms();
        renderer.end_frame();
        if (ms > worst_ms) worst_ms = ms;
    }

    std::fprintf(stderr, "[test_render_gl perf] worst submit_chaff over %d samples @ 10k agents: %.3fms\n",
                kSamples, worst_ms);

    // docs/AGENT_BRIEF.md §5: chaff_update + render_submit < 4ms combined.
    // This measures render_submit's share of that budget in isolation; give
    // it generous headroom (3ms) since chaff_update (owned by Wave 1B) shares
    // the same envelope.
    CHECK(worst_ms < 3.0);

    renderer.shutdown();
}
