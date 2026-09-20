#version 450 core
// Swarmer pass — vertex stage.
//
// One instanced quad per live swarmer in sim::SwarmerBuffers
// (src/sim/swarm/Swarmers.h), for every tower in the roster. Not a frozen
// contract: render::SwarmerGpuInstance is a Renderer.cpp implementation detail
// mirrored only here, the same arrangement projectile.vert and field.vert
// both have.
//
// WHY THIS IS NOT THE PROJECTILE PASS
// A round is a slug: it streaks along its velocity and reads as fired matter.
// A swarmer is a small living thing — it should read as a body that swims, and
// crucially it must stay legible when eighty of them are stacked on one lane.
// So the quad here is axis-aligned and square rather than a velocity-stretched
// capsule, and all of the motion lives in the fragment stage's wobble. A
// stretched swarmer at swarm density turns the cloud into a smear of dashes.

layout(location = 0) in vec2 a_corner;      // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;
layout(location = 2) in vec2  i_velocity;
layout(location = 3) in float i_radius;
layout(location = 4) in float i_phase;      // per-granule animation offset
layout(location = 5) in vec4  i_tint;
layout(location = 6) in uint  i_flags;      // bit 0: engaged; bits 8..11: kind; 12..15: tier
// ArborGrabber (kind 6): each arm = {reach in world units, relative angle,
// grip 0..1, phase (0 idle, 1 extending, 2 latching, 3 pulling, 4 recovery)}.
layout(location = 7) in vec4  i_arm0;
layout(location = 8) in vec4  i_arm1;
layout(location = 9) in vec4  i_arm2;

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2 v_local;
flat out vec4  v_tint;
flat out float v_phase;
flat out uint  v_flags;
flat out vec2  v_heading;
flat out vec4  v_arm0;
flat out vec4  v_arm1;
flat out vec4  v_arm2;
// World units per unit of v_local, so the fragment stage can put an arm tip
// given in world units into the quad's frame.
flat out float v_world_per_local;

// Generous, because the fragment stage pushes the membrane outward by up to
// ~18% of the radius and a tight quad would clip the bulges flat.
const float kPad = 1.6;

void main() {
    float speed = length(i_velocity);
    vec2  dir   = speed > 1e-5 ? i_velocity / speed : vec2(1.0, 0.0);

    // Match the tower's larger macrophage and leave room for its resting
    // pseudopods at every heading (the fragment shader rotates inside this
    // square). An arbor grabber's quad grows forward to contain the reach.
    uint kind = (i_flags >> 8u) & 15u;
    bool macrophage = kind == 2u || kind == 6u;
    float world_per_local = 2.0 * i_radius * (macrophage ? 1.35 : 1.0);
    float half_extent = macrophage ? 1.175 : 0.5 * kPad;
    if (kind == 6u) {
        float reach = max(i_arm0.x, max(i_arm1.x, i_arm2.x));
        half_extent = max(half_extent, (reach + 0.9 * i_radius) / world_per_local);
    }
    vec2 corner = a_corner * (2.0 * half_extent);
    vec2 offset = corner * world_per_local;

    v_local   = corner;
    v_tint    = i_tint;
    v_arm0    = i_arm0;
    v_arm1    = i_arm1;
    v_arm2    = i_arm2;
    v_world_per_local = world_per_local;
    // Each swarmer pulses on its own phase. Without the per-instance offset the
    // whole cloud breathes in lockstep and instantly reads as one object.
    v_phase   = i_phase + u_time * 6.0;
    v_flags   = i_flags;
    v_heading = dir;

    gl_Position = u_view_projection * vec4(i_position + offset, 0.0, 1.0);
}
