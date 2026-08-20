#version 450 core
// Goblet Cell fluid — pass 1 of 2, vertex stage.
//
// This pass does NOT draw the fluid. It accumulates a THICKNESS FIELD: one soft
// blob per simulated mucus particle, summed additively into an offscreen
// half-resolution buffer. fluid_composite.frag then finds the surface in that
// field and shades it. See render/Renderer.h's submit_fluid comment for why a
// fluid needs the two-stage treatment when nothing else in this renderer does.
//
// render::FluidGpuInstance is a Renderer.cpp implementation detail mirrored only
// here, the same arrangement swarmer.vert and projectile.vert both have.

layout(location = 0) in vec2 a_corner;      // shared unit quad, [-0.5, 0.5]

layout(location = 1) in vec2  i_position;
layout(location = 2) in vec2  i_motion;     // displacement over the last substep
layout(location = 3) in float i_radius;
layout(location = 4) in float i_density;    // relaxed density / rest density
layout(location = 5) in float i_fade;       // 0..1 lifetime fade
layout(location = 6) in float i_foam;       // 0..1 how churned this particle is

layout(location = 0) uniform mat4 u_view_projection;
layout(location = 1) uniform float u_time;

out vec2 v_local;          // position within the (unstretched) splat, +-0.5-ish
flat out float v_weight;   // how much thickness this particle deposits
flat out float v_foam;
flat out vec2  v_motion;

// The splat is drawn wider than the particle's physical spacing on purpose. The
// composite finds its surface at a threshold of the summed field, so if each
// blob only reached as far as the particle's own radius the field would dip
// below threshold in the gaps between neighbours and the body of the jet would
// come out perforated. Overlapping generously is what closes the surface.
const float kSplatScale = 3.1;

// Cap on how far a fast particle is stretched along its motion, as a multiple
// of the splat size. Unbounded stretching turns the leading edge of a burst
// into long thin needles that read as scratches rather than as liquid.
const float kMaxStretch = 2.2;

void main() {
    float speed = length(i_motion);
    vec2 dir = speed > 1e-6 ? i_motion / speed : vec2(1.0, 0.0);
    vec2 side = vec2(-dir.y, dir.x);

    float splat = i_radius * kSplatScale;

    // Motion stretch. Measured against the splat's own size so it is resolution
    // and tuning independent: a particle that moved one splat-width in a
    // substep gets roughly one extra splat-width of length.
    float stretch = clamp(1.0 + speed / max(splat, 1e-4), 1.0, kMaxStretch);
    // Conserve area while stretching, so a streaking particle does not also
    // deposit more mass than a resting one and brighten the leading edge.
    float squash = 1.0 / sqrt(stretch);

    vec2 corner = a_corner * 2.0;                        // -1..1
    vec2 offset = (dir * corner.x * stretch + side * corner.y * squash) * splat;

    v_local = corner;
    v_motion = i_motion;
    v_foam = i_foam;

    // Deposit weight. Fading particles thin out rather than vanishing, and a
    // particle the solver reports as UNDER rest density is at the free surface,
    // so it deposits a little less — which sharpens the boundary between the
    // body of a slug and the spray coming off it.
    v_weight = i_fade * mix(0.72, 1.0, clamp(i_density, 0.0, 1.0));

    gl_Position = u_view_projection * vec4(i_position + offset, 0.0, 1.0);
}
