// Measure the production GLSL centreline itself, rather than a C++ copy of it.
// A slithering body should move through a track whose overlapping ground
// coordinates stay put; normal track drift is the worm equivalent of foot slip.
#include "core/Math.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/burrow/Burrow.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <glad/glad.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;

namespace {

using Sample = std::array<f32, 4>;

std::string function_source(const std::string& fragment, const char* signature) {
    const usize start = fragment.find(signature);
    REQUIRE(start != std::string::npos);
    const usize open = fragment.find('{', start);
    REQUIRE(open != std::string::npos);
    i32 depth = 1;
    usize end = open + 1;
    while (end < fragment.size() && depth > 0) {
        if (fragment[end] == '{') ++depth;
        if (fragment[end] == '}') --depth;
        ++end;
    }
    REQUIRE(depth == 0);
    return fragment.substr(start, end - start);
}

std::vector<Sample> run_sampler(const std::string& source, const std::vector<Sample>& input) {
    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* raw = source.c_str();
    glShaderSource(shader, 1, &raw, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    std::array<char, 4096> log{};
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
    INFO(log.data());
    REQUIRE(compiled == GL_TRUE);

    const GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
    INFO(log.data());
    REQUIRE(linked == GL_TRUE);
    glDeleteShader(shader);

    std::vector<Sample> output(input.size());
    GLuint buffers[2]{};
    glCreateBuffers(2, buffers);
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(input.size() * sizeof(Sample));
    glNamedBufferData(buffers[0], bytes, input.data(), GL_STATIC_DRAW);
    glNamedBufferData(buffers[1], bytes, nullptr, GL_STATIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[1]);
    glUseProgram(program);
    glDispatchCompute(static_cast<GLuint>(input.size()), 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetNamedBufferSubData(buffers[1], 0, bytes, output.data());
    glUseProgram(0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glDeleteBuffers(2, buffers);
    glDeleteProgram(program);
    REQUIRE(glGetError() == GL_NO_ERROR);
    return output;
}

// Inputs are (local ground x, phase, instance scale, old sizing flag). Outputs
// are (world y, declared dy/dx, geometric dy/dx, unused). The metric uses the
// geometric tangent so an incorrect old shader derivative cannot bias it.
// std430 vec4 keeps both strides 16.
std::vector<Sample> sample_path(const std::string& fragment, const std::vector<Sample>& input) {
    const std::string source = std::string(R"glsl(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Inputs { vec4 inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { vec4 outputs[]; };
)glsl") + function_source(fragment, "float worm_path(") + R"glsl(
void main() {
    uint i = gl_GlobalInvocationID.x;
    vec4 v = inputs[i];
    float wave_scale = v.w > 0.5 ? 9.0 : v.z;
    vec4 shape = vec4(0.1, 6.28318530718 * wave_scale / 8.0, 0.975, 0.03);
    vec4 extra = vec4(48.0, 1.4, 0.18, 0.05);
    float slope;
    float y = worm_path(v.x, v.y, shape, extra, slope);
    float unused_slope;
    float right = worm_path(v.x + 0.0002, v.y, shape, extra, unused_slope);
    float left = worm_path(v.x - 0.0002, v.y, shape, extra, unused_slope);
    outputs[i] = vec4(y * v.z, slope, (right - left) / 0.0004, 0.0);
}
)glsl";
    return run_sampler(source, input);
}

// Inputs add local curvature in w. Outputs are the forward-mapped position in
// world units and the inverse transform's round-trip error in local units.
std::vector<Sample> sample_curved_path(const std::string& fragment, const std::vector<Sample>& input) {
    const std::string source = std::string(R"glsl(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Inputs { vec4 inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { vec4 outputs[]; };
float v_curvature;
)glsl") + function_source(fragment, "float worm_path(") +
        function_source(fragment, "vec2 worm_local_to_spine(") +
        function_source(fragment, "vec2 worm_spine_to_local(") + R"glsl(
void main() {
    uint i = gl_GlobalInvocationID.x;
    vec4 v = inputs[i];
    v_curvature = v.w;
    vec4 shape = vec4(0.1, 6.28318530718 * v.z / 8.0, 0.975, 0.03);
    vec4 extra = vec4(48.0, 1.4, 0.18, 0.05);
    float slope;
    vec2 spine = vec2(v.x, worm_path(v.x, v.y, shape, extra, slope));
    vec2 local = worm_spine_to_local(spine);
    vec2 inverse = worm_local_to_spine(local);
    outputs[i] = vec4(local * v.z, inverse - spine);
}
)glsl";
    return run_sampler(source, input);
}

struct Drift {
    f64 rms_ratio = 0.0;
    f64 max_ratio = 0.0;
    f64 max_world = 0.0;
};

// Check the outward nose against a circle in the actual Cartesian frame.
// The points are constructed from the independently evaluated endpoint/tangent,
// then passed through the production signed-distance function, including its
// endpoint projection and body-radius logic. The body junction at +/-90 degrees
// is excluded because genuine neighbouring bends can overlap it.
std::vector<Sample> sample_head_caps(const std::string& fragment,
                                     const std::vector<Sample>& input) {
    const std::string source = std::string(R"glsl(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Inputs { vec4 inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { vec4 outputs[]; };
float v_curvature;
)glsl") + function_source(fragment, "float worm_path(") +
        function_source(fragment, "vec2 worm_local_to_spine(") +
        function_source(fragment, "vec2 worm_spine_to_local(") +
        function_source(fragment, "float sdf_worm(") + R"glsl(
void main() {
    uint i = gl_GlobalInvocationID.x;
    vec4 v = inputs[i]; // phase, instance scale, local curvature, body shift
    v_curvature = v.z;
    vec4 shape = vec4(0.1, 6.28318530718 * v.y / 8.0, 0.975, 0.03);
    vec4 extra = vec4(48.0, 1.4, 0.18, 0.05);
    float s = 0.5 * shape.z + v.w;
    float slope;
    float wave = worm_path(s, v.x, shape, extra, slope);
    vec2 head = worm_spine_to_local(vec2(s, wave));
    float angle = v_curvature * s;
    vec2 tangent = normalize(mat2(cos(angle), sin(angle), -sin(angle), cos(angle)) *
                             vec2(1.0 - v_curvature * wave, slope));
    vec2 normal = vec2(-tangent.y, tangent.x);
    float radius = shape.w * 1.15;
    float sum_sq = 0.0, maximum = 0.0, sign_errors = 0.0;
    for (int direction = -2; direction <= 2; ++direction) {
        float a = float(direction) * 0.5235987756; // +/-60, +/-30, 0 degrees
        vec2 radial = cos(a) * tangent + sin(a) * normal;
        for (int shell = -1; shell <= 1; ++shell) {
            float distance = radius * (1.0 + 0.1 * float(shell));
            vec2 p = head + distance * radial;
            float u, lateral;
            float actual = sdf_worm(p, v.x, shape, extra, v.w, 0.0, u, lateral);
            float expected = distance - radius;
            float error = abs(actual - expected) / radius;
            sum_sq += error * error;
            maximum = max(maximum, error);
            if ((shell < 0 && actual >= 0.0) || (shell > 0 && actual <= 0.0))
                sign_errors += 1.0;
        }
    }
    outputs[i] = vec4(sum_sq / 15.0, maximum, sign_errors, abs(slope));
}
)glsl";
    return run_sampler(source, input);
}

Drift measure_drift(const std::string& fragment, bool old_clock = false) {
    std::vector<Sample> input;
    std::vector<f32> travelled;
    constexpr f32 kWorldK = math::kTwoPi / 8.0f;
    // Include normal/slow/fast motion, both spawn-size extremes, and crossings
    // of both the old 2pi wrap and the full new 4pi shape period.
    for (f32 scale : {6.75f, 9.0f, 11.25f}) {
        for (f32 speed : {1.0f, 3.0f, 10.125f, 20.0f}) {
            for (f32 initial_phase : {0.0f, 0.7f, 6.25f, 6.3f, 12.55f}) {
                const f32 distance = speed / 60.0f;
                const f32 advance = kWorldK * distance +
                                    (old_clock ? 0.25f * math::kTwoPi / 60.0f : 0.0f);
                const f32 period = (old_clock ? 1.0f : 2.0f) * math::kTwoPi;
                const f32 phase = std::fmod(initial_phase, period);
                const f32 after = std::fmod(phase + advance, period);
                for (i32 point = 0; point < 25; ++point) {
                    const f32 gx = (-0.32f + 0.64f * static_cast<f32>(point) / 24.0f) * 0.975f;
                    const f32 old_size = old_clock ? 1.0f : 0.0f;
                    input.push_back({gx, phase, scale, old_size});
                    input.push_back({gx - distance / scale, after, scale, old_size});
                    travelled.push_back(distance);
                }
            }
        }
    }
    const auto output = sample_path(fragment, input);
    Drift drift;
    f64 sum_sq = 0.0;
    for (usize pair = 0; pair < travelled.size(); ++pair) {
        const Sample& before = output[2 * pair];
        const Sample& after = output[2 * pair + 1];
        const f64 slope = 0.5 * (static_cast<f64>(before[2]) + after[2]);
        const f64 normal_drift = std::fabs(static_cast<f64>(after[0]) - before[0]) /
                                 std::sqrt(1.0 + slope * slope);
        const f64 ratio = normal_drift / travelled[pair];
        sum_sq += ratio * ratio;
        drift.max_ratio = std::max(drift.max_ratio, ratio);
        drift.max_world = std::max(drift.max_world, normal_drift);
    }
    drift.rms_ratio = std::sqrt(sum_sq / static_cast<f64>(travelled.size()));
    return drift;
}

std::string capture_path() {
    const char* specified = std::getenv("IMMUNE_SLITHER_CAPTURE");
    if (specified != nullptr && specified[0] != '\0') return specified;
    const char* temp = std::getenv("TEMP");
    return std::string(temp != nullptr ? temp : ".") + "/render_verify_slither_track.png";
}

// Default size and shipped speed, uniformly sampling phases that do not wrap.
Drift measure_ordinary_travel(const std::string& fragment, bool old_clock = false) {
    std::vector<Sample> input;
    constexpr f32 distance = 10.125f / 60.0f;
    constexpr f32 move_phase = math::kTwoPi * distance / 8.0f;
    constexpr f32 old_idle_phase = 0.25f * math::kTwoPi / 60.0f;
    const f32 advance = move_phase + (old_clock ? old_idle_phase : 0.0f);
    for (i32 phase_index = 0; phase_index < 100; ++phase_index) {
        const f32 phase = (math::kTwoPi - move_phase - old_idle_phase) *
                          (static_cast<f32>(phase_index) + 0.5f) / 100.0f;
        for (i32 point = 0; point < 25; ++point) {
            const f32 gx = (-0.32f + 0.64f * static_cast<f32>(point) / 24.0f) * 0.975f;
            const f32 old_size = old_clock ? 1.0f : 0.0f;
            input.push_back({gx, phase, 9.0f, old_size});
            input.push_back({gx - distance / 9.0f, phase + advance, 9.0f, old_size});
        }
    }
    const auto output = sample_path(fragment, input);
    Drift drift;
    f64 sum_sq = 0.0;
    for (usize pair = 0; pair < output.size() / 2; ++pair) {
        const Sample& before = output[2 * pair];
        const Sample& after = output[2 * pair + 1];
        const f64 slope = 0.5 * (static_cast<f64>(before[2]) + after[2]);
        const f64 normal = std::fabs(static_cast<f64>(after[0]) - before[0]) / std::sqrt(1.0 + slope * slope);
        const f64 ratio = normal / distance;
        sum_sq += ratio * ratio;
        drift.max_ratio = std::max(drift.max_ratio, ratio);
        drift.max_world = std::max(drift.max_world, normal);
    }
    drift.rms_ratio = std::sqrt(sum_sq / static_cast<f64>(output.size() / 2));
    return drift;
}

} // namespace

TEST_CASE("slither shader keeps its entire centreline on the travelled ground track",
          "[render][gl][slither]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 32, 32)) {
        WARN("headless GL unavailable; slither metric was not measured");
        return;
    }
    const auto fragment = platform::read_text_file(platform::asset_path("shaders/chaff.frag"));
    REQUIRE(fragment.has_value());
    const Drift drift = measure_drift(*fragment);
    std::fprintf(stderr, "[slither] production GLSL: normal track drift RMS %.6f%%, max %.6f%%, "
                         "max %.8f world units/tick (1500 point pairs)\n",
                 drift.rms_ratio * 100.0, drift.max_ratio * 100.0, drift.max_world);
    CHECK(drift.rms_ratio < 0.001);
    CHECK(drift.max_ratio < 0.005);
    CHECK(drift.max_world < 0.00005);
    const Drift ordinary = measure_ordinary_travel(*fragment);
    std::fprintf(stderr, "[slither] ordinary travel at size 1 and speed 10.125: normal track drift "
                         "RMS %.6f%%, max %.6f%% (2500 non-wrapping point pairs)\n",
                 ordinary.rms_ratio * 100.0, ordinary.max_ratio * 100.0);

    // Optional same-machine before/after evidence. The baseline file is never
    // required by the regression test and is deliberately outside assets/.
    const char* baseline_path = std::getenv("IMMUNE_SLITHER_BASELINE");
    if (baseline_path != nullptr && baseline_path[0] != '\0') {
        const auto baseline = platform::read_text_file(baseline_path);
        REQUIRE(baseline.has_value());
        const Drift old = measure_drift(*baseline, true);
        std::fprintf(stderr, "[slither] baseline GLSL + clock: normal track drift RMS %.3f%%, "
                             "max %.3f%%, max %.6f world units/tick\n",
                     old.rms_ratio * 100.0, old.max_ratio * 100.0, old.max_world);
        const Drift old_ordinary = measure_ordinary_travel(*baseline, true);
        std::fprintf(stderr, "[slither] baseline ordinary travel at size 1 and speed 10.125: "
                             "normal track drift RMS %.3f%%, max %.3f%%\n",
                     old_ordinary.rms_ratio * 100.0, old_ordinary.max_ratio * 100.0);
    }
}

TEST_CASE("slither shader reports the actual tangent of the curved body",
          "[render][gl][slither]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 32, 32)) {
        WARN("headless GL unavailable; slither tangent was not measured");
        return;
    }
    const auto fragment = platform::read_text_file(platform::asset_path("shaders/chaff.frag"));
    REQUIRE(fragment.has_value());
    std::vector<Sample> input;
    constexpr f32 h = 0.0001f;
    for (f32 phase : {0.0f, 1.0f, 3.0f, 6.0f, 9.0f, 12.0f}) {
        for (i32 point = 0; point < 25; ++point) {
            const f32 gx = -0.45f + 0.9f * static_cast<f32>(point) / 24.0f;
            input.push_back({gx - h, phase, 9.0f, 0.0f});
            input.push_back({gx, phase, 9.0f, 0.0f});
            input.push_back({gx + h, phase, 9.0f, 0.0f});
        }
    }
    const auto output = sample_path(*fragment, input);
    f64 max_error = 0.0;
    for (usize i = 0; i < output.size(); i += 3) {
        const f64 finite_difference = (static_cast<f64>(output[i + 2][0]) - output[i][0]) /
                                      (2.0 * h * 9.0);
        max_error = std::max(max_error, std::fabs(finite_difference - output[i + 1][1]));
    }
    std::fprintf(stderr, "[slither] actual-GLSL tangent max finite-difference error %.6f\n", max_error);
    CHECK(max_error < 0.01);
}

TEST_CASE("slither shader follows the same curved ground track through a steady turn",
          "[render][gl][slither]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 32, 32)) {
        WARN("headless GL unavailable; curved slither was not measured");
        return;
    }
    const auto fragment = platform::read_text_file(platform::asset_path("shaders/chaff.frag"));
    REQUIRE(fragment.has_value());
    std::vector<Sample> input;
    std::vector<f32> travelled;
    std::vector<f32> curvature;
    for (f32 scale : {6.75f, 9.0f, 11.25f}) {
        for (f32 speed : {1.0f, 10.125f, 20.0f}) {
            for (f32 bend : {-0.12f, -0.04f, 0.0f, 0.04f, 0.12f}) {
                for (f32 phase : {0.7f, 12.55f}) {
                    const f32 distance = speed / 60.0f;
                    const f32 after = std::fmod(phase + math::kTwoPi * distance / 8.0f,
                                              2.0f * math::kTwoPi);
                    for (i32 point = 0; point < 25; ++point) {
                        const f32 gx = -0.32f + 0.64f * static_cast<f32>(point) / 24.0f;
                        input.push_back({gx, phase, scale, bend * scale});
                        input.push_back({gx - distance / scale, after, scale, bend * scale});
                        travelled.push_back(distance);
                        curvature.push_back(bend);
                    }
                }
            }
        }
    }
    const auto output = sample_curved_path(*fragment, input);
    f64 sum_sq = 0.0;
    f64 max_world = 0.0;
    f64 max_roundtrip_world = 0.0;
    for (usize pair = 0; pair < travelled.size(); ++pair) {
        const f64 distance = travelled[pair];
        const f64 bend = curvature[pair];
        const f64 angle = bend * distance;
        const f64 sn = std::sin(angle), cs = std::cos(angle);
        const f64 cx = bend == 0.0 ? distance : sn / bend;
        const f64 cy = bend == 0.0 ? 0.0 : 2.0 * std::pow(std::sin(0.5 * angle), 2.0) / bend;
        const Sample& before = output[2 * pair];
        const Sample& after = output[2 * pair + 1];
        const f64 dx = cx + cs * after[0] - sn * after[1] - before[0];
        const f64 dy = cy + sn * after[0] + cs * after[1] - before[1];
        const f64 error = std::sqrt(dx * dx + dy * dy);
        sum_sq += (error / distance) * (error / distance);
        max_world = std::max(max_world, error);
        for (usize offset = 0; offset < 2; ++offset) {
            const Sample& point = output[2 * pair + offset];
            const f64 scale = input[2 * pair + offset][2];
            const f64 roundtrip = std::hypot(static_cast<f64>(point[2]), static_cast<f64>(point[3])) * scale;
            max_roundtrip_world = std::max(max_roundtrip_world, roundtrip);
        }
    }
    const f64 rms_ratio = std::sqrt(sum_sq / static_cast<f64>(travelled.size()));
    std::fprintf(stderr, "[slither] curved production GLSL: ground track drift RMS %.6f%%, "
                         "max %.8f world units/tick, roundtrip max %.8f world units\n",
                 rms_ratio * 100.0, max_world, max_roundtrip_world);
    CHECK(rms_ratio < 0.001);
    CHECK(max_world < 0.00005);
    CHECK(max_roundtrip_world < 0.00005);
}

TEST_CASE("visual verification: parasite slithers along one track across four frames",
          "[render][gl][slither][screenshot]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 800, 450)) {
        WARN("headless GL unavailable; slither capture was not produced");
        return;
    }
    constexpr auto family = PathogenFamily::Parasite;
    const sim::SlitherParams saved_slither = sim::family_slither(family);
    const render::FamilyVisual saved_visual = render::family_visual(family);
    struct Restore {
        sim::SlitherParams slither;
        render::FamilyVisual visual;
        ~Restore() {
            sim::set_family_slither(PathogenFamily::Parasite, slither);
            render::set_family_visual(PathogenFamily::Parasite, visual);
        }
    } restore{saved_slither, saved_visual};
    sim::SlitherParams slither;
    slither.enabled = true;
    sim::set_family_slither(family, slither);
    render::set_family_visual(family, {9.0f, 1.0f, 0.0f});

    sim::ChaffBuffers chaff;
    chaff.reserve(4);
    for (i32 i = 0; i < 4; ++i) {
        sim::ChaffSpawnParams spawn;
        spawn.family = family;
        spawn.position = {2.0f, -7.5f + 5.0f * static_cast<f32>(i)};
        spawn.size_scale = i < 3 ? 0.75f + 0.25f * static_cast<f32>(i) : 1.0f;
        REQUIRE(chaff.spawn(spawn).valid());
        chaff.body_heading[i] = 0.0f;
    }

    sim::SpatialHash hash;
    sim::SpatialHashDesc hd;
    hd.bounds = {{-10.0f, -14.0f}, {24.0f, 14.0f}};
    hd.cell_size = 3.0f;
    hash.configure(hd);
    render::RendererDesc desc;
    desc.framebuffer_width = 800;
    desc.framebuffer_height = 450;
    desc.max_chaff_instances = 4;
    render::Renderer renderer;
    REQUIRE(renderer.init(desc));
    render::Camera camera;
    camera.set_viewport(800, 450);
    camera.set_center({6.0f, 2.5f});
    camera.set_view_height(28.0f);

    const auto render_at_distance = [&](f32 distance, i32 active_row = -1) {
        for (usize i = 0; i < chaff.count(); ++i) {
            chaff.pos_x[i] = chaff.prev_pos_x[i] = 2.0f + distance;
            chaff.slither_phase[i] = 0.7f + math::kTwoPi * distance / slither.wavelength;
            chaff.burrow_state[i] = active_row >= 0 && static_cast<i32>(i) != active_row
                                        ? sim::burrow_state::kUnderground : sim::burrow_state::kSurface;
            chaff.burrow_anim[i] = 0.0f;
        }
        // The last row follows a constant-radius bend. This is the same
        // motion measured above, now through the complete production renderer.
        constexpr f32 bend = 0.05f;
        const f32 angle = bend * distance;
        chaff.pos_x[3] = chaff.prev_pos_x[3] = 2.0f + std::sin(angle) / bend;
        chaff.pos_y[3] = chaff.prev_pos_y[3] = 7.5f + 2.0f * std::pow(std::sin(0.5f * angle), 2.0f) / bend;
        chaff.body_heading[3] = angle;
        chaff.slither_curvature[3] = bend;
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count());
        renderer.begin_frame(camera, 1.0f);
        renderer.submit_chaff(chaff, hash);
        renderer.end_frame();
        CHECK(renderer.stats().chaff_instances_drawn == (active_row >= 0 ? 1u : 4u));
        std::vector<u8> pixels;
        i32 width = 0, height = 0;
        REQUIRE(renderer.read_pixels(pixels, width, height));
        REQUIRE(width == 800);
        REQUIRE(height == 450);
        return pixels;
    };
    std::vector<u8> strip(1600 * 900 * 4);
    for (i32 frame = 0; frame < 4; ++frame) {
        const auto pixels = render_at_distance(2.0f * static_cast<f32>(frame));
        const usize tile_x = static_cast<usize>(frame % 2) * 800;
        const usize tile_y = static_cast<usize>(frame / 2) * 450;
        for (usize row = 0; row < 450; ++row) {
            std::copy_n(pixels.begin() + row * 800 * 4, 800 * 4,
                        strip.begin() + ((tile_y + row) * 1600 + tile_x) * 4);
        }
    }
    // Measure the complete Renderer/vertex/uniform path as well as the GLSL
    // function above. At a fixed ground x in the overlapping interior, a
    // brightness-weighted body centre must stay put despite its taper moving
    // through that point. The generous two-pixel limit allows changing skin
    // highlights and width, while detecting incorrect world/instance scaling.
    const auto body_center_y = [&](const std::vector<u8>& pixels, f32 x, f32 base_y) {
        const i32 column = static_cast<i32>(std::round(camera.world_to_screen({x, base_y}).x));
        const f32 edge_a = camera.world_to_screen({x, base_y - 3.0f}).y;
        const f32 edge_b = camera.world_to_screen({x, base_y + 3.0f}).y;
        const i32 lo = math::clamp(static_cast<i32>(std::floor(std::min(edge_a, edge_b))), 0, 449);
        const i32 hi = math::clamp(static_cast<i32>(std::ceil(std::max(edge_a, edge_b))), 0, 449);
        f64 weighted_y = 0.0, weight_sum = 0.0;
        for (i32 py = lo; py <= hi; ++py) {
            for (i32 px = column - 1; px <= column + 1; ++px) {
                REQUIRE(px >= 0);
                REQUIRE(px < 800);
                const usize offset = (static_cast<usize>(py) * 800 + static_cast<usize>(px)) * 4;
                const f64 weight = math::max(static_cast<i32>(pixels[offset + 2]) - 70, 0);
                weighted_y += weight * (static_cast<f64>(py) + 0.5);
                weight_sum += weight;
            }
        }
        REQUIRE(weight_sum > 0.0);
        return weighted_y / weight_sum;
    };
    f64 max_pixel_drift = 0.0;
    for (i32 row = 0; row < 3; ++row) {
        // Isolate the measured worm so another row's bend cannot contribute
        // bright pixels inside this one's scan region.
        const auto before_pixels = render_at_distance(0.0f, row);
        const auto after_pixels = render_at_distance(2.0f, row);
        const f32 size = 0.75f + 0.25f * static_cast<f32>(row);
        const f32 base_y = -7.5f + 5.0f * static_cast<f32>(row) + 0.16f * 9.0f * size;
        f64 row_max = 0.0;
        for (i32 sample = 0; sample < 41; ++sample) {
            const f32 x = 2.5f + 2.0f * static_cast<f32>(sample) / 40.0f;
            const f64 before = body_center_y(before_pixels, x, base_y);
            const f64 after = body_center_y(after_pixels, x, base_y);
            row_max = std::max(row_max, std::fabs(after - before));
        }
        max_pixel_drift = std::max(max_pixel_drift, row_max);
        std::fprintf(stderr, "[slither] actual Renderer size %.2f: overlapping centreline max drift %.3f pixels\n",
                     static_cast<f64>(size), row_max);
    }
    CHECK(max_pixel_drift < 2.0);
    const std::string out = capture_path();
    REQUIRE(render::write_png_rgba(out, strip.data(), 1600, 900));
    std::fprintf(stderr, "[slither] four fixed-camera frames (reading order), sizes .75/1/1.25 "
                         "plus curved row, 2 world units travelled each: %s\n", out.c_str());
    const char* frames_dir = std::getenv("IMMUNE_SLITHER_FRAMES");
    if (frames_dir != nullptr && frames_dir[0] != '\0') {
        REQUIRE(platform::ensure_directory(frames_dir));
        for (i32 frame = 0; frame < 60; ++frame) {
            const auto pixels = render_at_distance(10.125f * static_cast<f32>(frame) / 60.0f);
            char name[32];
            std::snprintf(name, sizeof(name), "/frame_%03d.png", frame);
            REQUIRE(render::write_png_rgba(std::string(frames_dir) + name, pixels.data(), 800, 450));
        }
        std::fprintf(stderr, "[slither] 60 actual-render frames at shipped speed: %s\n", frames_dir);
    }
    renderer.shutdown();
}

TEST_CASE("slither head remains circular at steep tangents and on curved backbones",
          "[render][gl][slither][slither-head]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 32, 32)) {
        WARN("headless GL unavailable; head roundness was not measured");
        return;
    }
    const auto fragment = platform::read_text_file(platform::asset_path("shaders/chaff.frag"));
    REQUIRE(fragment.has_value());
    std::vector<Sample> input;
    for (f32 scale : {6.75f, 9.0f, 11.25f}) {
        for (f32 bend : {-1.2f, 0.0f, 1.2f}) {
            for (f32 shift : {-0.5f, 0.0f, 0.5f}) {
                for (i32 phase = 0; phase < 96; ++phase) {
                    input.push_back({2.0f * math::kTwoPi * static_cast<f32>(phase) / 96.0f,
                                     scale, bend, shift});
                }
            }
        }
    }
    const auto measure = [&](const std::string& source, bool verify) {
        const auto samples = sample_head_caps(source, input);
        f64 mean_sq = 0.0, max_error = 0.0;
        u32 sign_errors = 0, steep_cases = 0;
        for (const auto& sample : samples) {
            mean_sq += sample[0];
            max_error = std::max(max_error, static_cast<f64>(sample[1]));
            sign_errors += static_cast<u32>(sample[2]);
            if (sample[3] > 1.0f) ++steep_cases;
        }
        const f64 rms = std::sqrt(mean_sq / static_cast<f64>(samples.size()));
        std::fprintf(stderr, "[slither head] %s: outward cap radius error RMS %.6f%%, "
                             "max %.6f%%; inside/outside errors %u; "
                             "%zu radial samples (%u steep poses)\n",
                     verify ? "production GLSL" : "baseline GLSL",
                     100.0 * rms, 100.0 * max_error, sign_errors, input.size() * 15, steep_cases);
        if (verify) {
            CHECK(steep_cases > 500);
            CHECK(sign_errors == 0);
            CHECK(max_error < 0.0025); // quarter percent of the head radius
        }
    };
    measure(*fragment, true);
    const char* baseline_path = std::getenv("IMMUNE_SLITHER_HEAD_BASELINE");
    if (baseline_path != nullptr && baseline_path[0] != '\0') {
        const auto baseline = platform::read_text_file(baseline_path);
        REQUIRE(baseline.has_value());
        measure(*baseline, false);
    }
}

TEST_CASE("visual verification: parasite heads stay round in steep and curved poses",
          "[render][gl][slither][slither-head][screenshot]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 1200, 900)) {
        WARN("headless GL unavailable; head poses were not captured");
        return;
    }
    constexpr auto family = PathogenFamily::Parasite;
    struct Restore {
        sim::SlitherParams slither = sim::family_slither(PathogenFamily::Parasite);
        render::FamilyVisual visual = render::family_visual(PathogenFamily::Parasite);
        ~Restore() {
            sim::set_family_slither(PathogenFamily::Parasite, slither);
            render::set_family_visual(PathogenFamily::Parasite, visual);
        }
    } restore;
    sim::SlitherParams slither;
    slither.enabled = true;
    sim::set_family_slither(family, slither);
    render::set_family_visual(family, {9.0f, 1.0f, 0.0f});

    sim::ChaffBuffers chaff;
    chaff.reserve(9);
    for (i32 row = 0; row < 3; ++row) {
        for (i32 column = 0; column < 3; ++column) {
            sim::ChaffSpawnParams spawn;
            spawn.family = family;
            spawn.position = {-15.0f + 15.0f * static_cast<f32>(column),
                               10.0f - 10.0f * static_cast<f32>(row)};
            spawn.size_scale = 0.75f + 0.25f * static_cast<f32>(column);
            REQUIRE(chaff.spawn(spawn).valid());
            const usize i = chaff.count() - 1;
            const f32 scale = 9.0f * spawn.size_scale;
            // These arguments maximize positive/negative primary head slope.
            const f32 arg = row == 1 ? 4.0941235f : 12.3049901f;
            chaff.slither_phase[i] = std::fmod(arg - math::kTwoPi * scale * 0.5f * slither.body_length /
                                                       slither.wavelength + sim::kSlitherPhasePeriod,
                                             sim::kSlitherPhasePeriod);
            chaff.body_heading[i] = 0.0f;
            chaff.slither_curvature[i] = row == 2
                                            ? (-0.8f + 0.8f * static_cast<f32>(column)) / scale
                                            : 0.0f;
        }
    }
    sim::SpatialHash hash;
    sim::SpatialHashDesc hd;
    hd.bounds = {{-30.0f, -20.0f}, {30.0f, 22.0f}};
    hd.cell_size = 3.0f;
    hash.configure(hd);
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count());
    render::RendererDesc desc;
    desc.framebuffer_width = 1200;
    desc.framebuffer_height = 900;
    desc.max_chaff_instances = 12;
    render::Renderer renderer;
    REQUIRE(renderer.init(desc));
    render::Camera camera;
    camera.set_viewport(1200, 900);
    camera.set_center({0.0f, 1.0f});
    camera.set_view_height(34.0f);
    renderer.begin_frame(camera, 1.0f);
    renderer.submit_chaff(chaff, hash);
    renderer.end_frame();
    CHECK(renderer.stats().chaff_instances_drawn == 9);
    std::vector<u8> pixels;
    i32 width = 0, height = 0;
    REQUIRE(renderer.read_pixels(pixels, width, height));
    REQUIRE(width == 1200);
    REQUIRE(height == 900);
    const char* requested = std::getenv("IMMUNE_SLITHER_HEAD_CAPTURE");
    const char* temp = std::getenv("TEMP");
    const std::string out = requested != nullptr && requested[0] != '\0'
                                ? std::string(requested)
                                : std::string(temp != nullptr ? temp : ".") + "/render_verify_slither_heads.png";
    REQUIRE(render::write_png_rgba(out, pixels.data(), width, height));
    std::fprintf(stderr, "[slither head] steep positive/negative slope and curved poses, "
                         "sizes .75/1/1.25 by column: %s\n", out.c_str());
    renderer.shutdown();
}
