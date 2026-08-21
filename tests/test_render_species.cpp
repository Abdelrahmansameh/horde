// tests/test_render_species.cpp — per-species silhouette verification.
//
// chaff.frag gives each pathogen family its own SDF silhouette (a spiked
// icosahedral capsid for a virus, a flagellated rod for a bacterium, ...)
// rather than tinting one shared disc. That is easy to break silently: a
// mis-packed family id, a shader edit, or a batcher change all still produce
// perfectly plausible coloured blobs, and the failure only shows up if someone
// happens to look closely at the right family.
//
// So these tests do two things:
//   1. Render a clean, isolated grid per family and write a PNG for eyeballing,
//      with the camera framed tight enough that a single agent is hundreds of
//      pixels across instead of the ~8 it gets at gameplay zoom.
//   2. Assert the silhouettes are measurably DIFFERENT from each other, which
//      is the part that keeps working after nobody is looking any more.
#include "core/Math.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::render;

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
    ~HeadlessGl() { window.destroy(); }
};

std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

/// A tidy row of one family, well separated so no two silhouettes touch and
/// the LOD blob pass never engages (that pass exists for packed crowds and
/// would hide exactly what these tests are checking).
void spawn_row(ChaffBuffers& chaff, PathogenFamily family, Vec2 origin, u32 n, f32 spacing) {
    for (u32 i = 0; i < n; ++i) {
        if (chaff.full()) return;
        ChaffSpawnParams p;
        p.family = family;
        p.position = Vec2{origin.x + static_cast<f32>(i) * spacing, origin.y};
        // A fixed non-axis-aligned heading: the vertex stage rotates local
        // space by velocity direction, so an elongated species (the bacterial
        // rod) must be given one or it always renders end-on.
        p.velocity = Vec2{2.0f, 0.9f};
        p.density = 1.0f;
        chaff.spawn(p);
    }
}

/// Coverage = fraction of pixels the family's silhouettes actually paint.
/// A spiked capsid and a slim rod fill their quads very differently, so this
/// is a cheap, stable proxy for "these are not the same shape".
f32 render_and_measure(Renderer& renderer, const Camera& camera, const ChaffBuffers& chaff,
                       const SpatialHash& hash, const std::string& out_path) {
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_chaff(chaff, hash);
    renderer.end_frame();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    if (!renderer.read_pixels(pixels, w, h)) return -1.0f;
    write_png_rgba(out_path, pixels.data(), w, h);

    // The clear colour is a very dark plum; anything an agent draws is far
    // brighter than the background or its own soft drop shadow.
    usize lit = 0;
    for (usize i = 0; i + 3 < pixels.size(); i += 4) {
        const u32 sum = static_cast<u32>(pixels[i]) + pixels[i + 1] + pixels[i + 2];
        if (sum > 150u) ++lit;
    }
    const usize total = pixels.size() / 4;
    std::fprintf(stderr, "[species] %s coverage=%.4f\n", out_path.c_str(),
                 static_cast<f64>(lit) / static_cast<f64>(total));
    return total == 0 ? -1.0f : static_cast<f32>(lit) / static_cast<f32>(total);
}

SpatialHash make_hash(Rect bounds) {
    SpatialHash hash;
    SpatialHashDesc d;
    d.bounds = bounds;
    d.cell_size = 4.0f;
    hash.configure(d);
    return hash;
}

f32 render_family(Renderer& renderer, PathogenFamily family, const std::string& name) {
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{40.0f, 20.0f}};
    ChaffBuffers chaff;
    chaff.reserve(64);
    spawn_row(chaff, family, Vec2{8.0f, 10.0f}, 4, 6.0f);

    SpatialHash hash = make_hash(bounds);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);

    Camera camera;
    camera.set_viewport(1200, 400);
    camera.set_bounds(bounds);
    camera.set_center(Vec2{17.0f, 10.0f});
    camera.set_view_height(9.0f);   // tight: one agent is ~150px tall
    camera.clamp_to_bounds();

    return render_and_measure(renderer, camera, chaff, hash,
                              scratch_path("immune_species_" + name + ".png"));
}

} // namespace

TEST_CASE("virus and bacteria render as distinct species silhouettes",
          "[render][gl][species]") {
    HeadlessGl gl(1200, 400);
    if (!gl.ok) {
        WARN("headless GL unavailable; skipping");
        return;
    }
    RendererDesc rd;
    rd.framebuffer_width = 1200;
    rd.framebuffer_height = 400;
    Renderer renderer;
    REQUIRE(renderer.init(rd));

    const f32 virus = render_family(renderer, PathogenFamily::Virus, "virus");
    const f32 bacteria = render_family(renderer, PathogenFamily::Bacteria, "bacteria");

    REQUIRE(virus > 0.0f);
    REQUIRE(bacteria > 0.0f);

    // Each family must actually paint something substantial -- a shader that
    // discards everything would otherwise "pass" a pure inequality check.
    REQUIRE(virus > 0.002f);
    REQUIRE(bacteria > 0.002f);

    // The real assertion: these are different SHAPES, not one disc in two
    // colours. Virus and bacteria have nearly the same authored silhouette
    // scale, so a meaningful coverage gap between them can only come from
    // geometry -- a spiked capsid and a slim flagellated rod fill their quads
    // very differently. A dropped or mis-packed family id would collapse both
    // onto the shader's round default and close this gap.
    const f32 ratio = virus > bacteria ? virus / bacteria : bacteria / virus;
    INFO("virus=" << virus << " bacteria=" << bacteria);
    REQUIRE(ratio > 1.15f);

    renderer.shutdown();
}
