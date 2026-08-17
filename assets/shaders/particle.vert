#version 450 core
// Cosmetic particle pass — vertex stage. Owner: Wave 6D.
//
// CONTRACT: the instance attribute layout below mirrors vfx::ParticleInstance
// (src/vfx/Particles.h) byte for byte. Changing one without the other is a
// contract break. 48 bytes per instance, one instanced draw per BlendMode,
// up to RendererDesc::max_particle_instances (262144) per draw.
//
// THE VERTEX STAGE ONLY BUILDS THE QUAD. Every per-kind *look* lives in
// particle.frag; this stage's job is to give the fragment stage a normalized
// local frame plus enough shape information to do SDF math in it:
//
//   v_local.y  is always in width units, true edge at |y| = 0.5.
//   v_local.x  is along the shape's major axis, true edge at |x| = 0.5*aspect.
//   v_aspect   = major/minor extent. 1.0 for the round kinds, large for the
//               directional ones. The fragment stage's capsule SDF degenerates
//               to a circle at aspect == 1, so one distance expression serves
//               every kind.
//
// DIRECTIONAL KINDS AND i_velocity
// Tracer/Beam/Bolt orient along i_velocity — that is exactly why velocity is
// in the frozen instance layout (see Particles.h: "Shader uses velocity to
// orient streaks/beams"). The layout carries no endpoint, so:
//   Tracer: velocity is a real world velocity. The streak is a motion-blur
//           exposure of it (kStreakSeconds), so fast rounds streak long and
//           slow embers stay dots. The streak trails BEHIND the instance
//           position, which stays the bright head.
//   Beam/Bolt: velocity is read as the SEGMENT VECTOR from the particle's
//           position to its far endpoint (|velocity| = segment length). A
//           near-zero velocity falls back to a short i_size-derived segment so
//           a degenerate instance still draws something instead of vanishing.
//
// age_norm drives the shader-side scale curves here (ring expansion, mist
// swell, spark shrink); the alpha curves live in the fragment stage.

layout(location = 0) in vec2 a_corner;      // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;   // offset  0
layout(location = 2) in vec2  i_velocity;   // offset  8
layout(location = 3) in float i_size;       // offset 16
layout(location = 4) in float i_rotation;   // offset 20
layout(location = 5) in vec4  i_tint;       // offset 24 : tint_rgba8, normalized
layout(location = 6) in uint  i_kind_blend; // offset 28 : kind | blend << 16
layout(location = 7) in float i_age_norm;   // offset 32
layout(location = 8) in float i_seed;       // offset 36

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2 v_local;
flat out vec4  v_tint;
flat out uint  v_kind;
flat out float v_age;
flat out float v_seed;
flat out float v_aspect;

// vfx::ParticleKind, low 16 bits of kind_blend. Order is frozen.
const uint kTracer = 0u;
const uint kSpark  = 1u;
const uint kRing   = 2u;
const uint kShard  = 3u;
const uint kMist   = 4u;
const uint kBeam   = 5u;
const uint kBolt   = 6u;

// Soft glow needs to bleed past the shape's true edge, so the quad is a little
// larger than the shape it holds — same trick chaff.vert/field.vert use.
const float kPad = 1.35;

// Motion-blur exposure for Tracer streaks. ~2 frames at 60 Hz: long enough to
// read as speed, short enough that a stationary ember is still a dot.
const float kStreakSeconds = 0.035;

void main() {
    uint  kind = i_kind_blend & 0xFFFFu;
    float age  = clamp(i_age_norm, 0.0, 1.0);

    float speed = length(i_velocity);
    vec2  dir   = speed > 1e-5 ? i_velocity / speed : vec2(1.0, 0.0);

    // Defaults: a round, unoriented blob at the instance's own rotation.
    vec2  axis   = vec2(cos(i_rotation), sin(i_rotation));
    vec2  center = i_position;
    float half_l = 0.5 * i_size;
    float half_w = 0.5 * i_size;

    if (kind == kTracer) {
        float streak = min(speed * kStreakSeconds, i_size * 10.0);
        half_l = 0.5 * (i_size + streak);
        half_w = 0.5 * i_size * (1.0 - 0.45 * age);
        axis   = dir;
        center = i_position - dir * (0.5 * streak);
    } else if (kind == kBeam || kind == kBolt) {
        // SEGMENT KINDS. For these two i_size is the segment's LENGTH, not a
        // diameter — vfx/Particles.cpp's BeamFired and ChainArc cases both
        // spawn with `velocity = unit direction, size = segment length`, and
        // the endpoint has nowhere else to travel (ParticleInstance packs the
        // direction into vx/vy and has no endpoint field).
        //
        // This branch used to read i_size as a WIDTH as well, via
        //     len    = max(speed, i_size * 4.0)
        //     half_w = 0.5 * max(i_size, len * 0.012)
        // and since the emitter's velocity is a unit vector, speed is always 1
        // and the i_size*4 fallback always won. A 20-unit beam was therefore
        // drawn 80 units long and 20 units WIDE — the exact opposite of the
        // hairline this file and particle.frag both promise, and the reason the
        // Laser rendered as a blown-out white smear four times longer than the
        // beam it was supposed to be showing. The Bolt overshot the same way.
        float len = max(i_size, 1e-3);
        half_l = 0.5 * len;
        // Hairline for a Beam (a clean straight line of energy), broad for a
        // Bolt (wide enough to hold the jagged centreline AND its branch). The
        // ~25x ratio between them is structural: it is why a Bolt can never be
        // mistaken for a Beam, and particle.frag's header cites it. The floors
        // stop a very short segment collapsing to sub-pixel.
        half_w = (kind == kBeam) ? 0.5 * max(len * 0.012, 0.05)
                                 : 0.5 * max(len * 0.30, 0.10);
        axis   = dir;
        center = i_position + dir * (0.5 * len);
    } else if (kind == kRing) {
        // Ease-out expansion: fast punch outward, then settle. Shockwave read.
        float grow = 1.0 - (1.0 - age) * (1.0 - age);
        half_l = half_w = 0.5 * i_size * mix(0.15, 1.0, grow);
    } else if (kind == kMist) {
        half_l = half_w = 0.5 * i_size * (1.0 + 0.9 * age); // drifts and swells
    } else if (kind == kSpark) {
        half_l = half_w = 0.5 * i_size * (1.0 - 0.30 * age);
    }
    // kShard keeps i_size and spins with i_rotation (CPU-integrated spin).

    float aspect = half_l / max(half_w, 1e-6);

    vec2 corner = a_corner * kPad;
    vec2 perp   = vec2(-axis.y, axis.x);
    vec2 offset = axis * (corner.x * 2.0 * half_l) + perp * (corner.y * 2.0 * half_w);

    v_local  = vec2(corner.x * aspect, corner.y);
    v_tint   = i_tint;
    v_kind   = kind;
    v_age    = age;
    v_seed   = i_seed;
    v_aspect = aspect;

    gl_Position = u_view_projection * vec4(center + offset, 0.0, 1.0);
}
