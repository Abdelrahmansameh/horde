# Rendering, camera and effects

IMMUNE draws a 2D simulation through OpenGL 4.5 using procedural shader shapes,
instanced quads and a tilted orthographic camera. Rendering consumes gameplay
state and cannot change it. Normal deployed immune cells are in the swarmer
pass; legacy spawner towers remain available in the ECS entity pass.

Start with [Renderer.h](../src/render/Renderer.h),
[Renderer.cpp](../src/render/Renderer.cpp), and the caller in
[App::render_frame](../src/app/App.cpp). Module ownership is in
[ARCHITECTURE.md](ARCHITECTURE.md); GUI rendering is documented separately in
[UI_FRAMEWORK.md](UI_FRAMEWORK.md).

## Current frame and defaults

`begin_frame()` clears the default framebuffer, captures camera matrices/view
bounds/interpolation alpha, resets frame statistics, and updates cosmetic time
from the renderer wall clock. The interactive world draw order is:

| Order | Submission | Main shaders / purpose |
| --- | --- | --- |
| 1 | Tissue | `tissue.vert/frag`: lumen/substrate, vessel identity, flow-aligned plasma |
| 2 | Chaff and brief death bodies | `chaff.vert/frag`: per-family pathogen batches; optional `blob.vert/frag` mass LOD |
| 3 | ECS entities | `entity.vert/frag`: named agents, barriers/scars and legacy tower bodies/overlays |
| 4 | Damage-field snapshot | `field.vert/frag`: circle/rect/cone/chain visualizations |
| 5 | Friendly rounds, then hostile toxin shots | `projectile.vert/frag`: real simulated shots |
| 6 | Swarmers | `swarmer.vert/frag`: deployed cells and legacy released units, arms/latches/build state |
| 7 | Fluid thickness and surface composite | `fluid.vert/frag`, `fluid_composite.vert/frag`: real mucus particles reconstructed as liquid |
| 8 | Cosmetic particles | `particle.vert/frag`: additive first, then alpha blend |
| 9 | Optional flow/squad debug lines | `flow_debug.vert/frag` |
| 10 | GUI and developer overlays | Retained player GUI plus ImGui developer tools |

Submission methods issue their draws; this is not a deferred sort of an arbitrary
scene graph. Within `submit_entities()`, legacy towers are ordered to overlay
the pathogen crowd. Fluid composites after units so the wet surface covers what
is underneath it. Particle blend order makes alpha matter/mist cover glow.

`end_frame()` currently calls `glFinish()`. Despite older header/design wording,
there is no active general HDR/bloom/tone-map graph in this function. The fluid
has its own offscreen target, not a whole-world postprocessing pipeline. Profile
the synchronization cost before changing it. App calls it after the world/debug
passes and before drawing the GUI; its `render_submit` timing includes that
wait, while renderer `FrameStats::submit_ms` sums individual CPU submission
timers.

Current defaults in `RendererDesc` are:

- Density blob LOD is **off**. Ordinary sprites carry the horde; the blob code
  and its unit tests remain available.
- `kShadowsEnabled` is **false**, controlling chaff/entity/swarmer drop shadows.
- Shader hot reload is descriptor-controlled and defaults off; App enables its
  development workflow through its renderer setup.
- Buffers have explicit capacities for each population. A larger sim capacity
  does not automatically resize every render stream.

Header comments retain historical counts and measurements. Current family and
roster counts come from `kFamilyCount` / `kTowerTypeCount`; there are three
pathogen families and five immune-cell types. Historical submission timings are
examples from particular hardware, not guaranteed costs for every build.

## Camera and interpolation

[Camera](../src/render/Camera.h) is a fixed-tilt top-down affine projection. Y is
foreshortened by tilt; no rotating 3D world geometry is required. Tilt is clamped
to 0–45 degrees, default 20. `view_height` controls world units visible through
the viewport. `world_to_screen()` / `screen_to_world()` convert with a top-left
pixel origin so placement/editor picks can invert the draw transform exactly.

Set viewport on resize and keep camera bounds aligned with play bounds.
`clamp_to_bounds()` and the reference-framing clamp helpers handle wide views
and zoom/pan limits. `visible_bounds()` supplies density-texture extent and
potential culling inputs; current `submit_chaff()` explicitly disables batcher
culling, so it is not proof that offscreen chaff is discarded.

Chaff instances interpolate `prev_pos_*` toward current positions using the
fixed-clock alpha. This presents a smoothed view of fixed simulation steps;
other stores have their own presentation paths and should not be assumed to
interpolate identically. Offline screenshot mode passes alpha **1** to show the
completed tick. Renderer animation time remains cosmetic wall time, so matching
simulation hashes does not imply pixel-identical captures. Camera tests:
[test_camera.cpp](../tests/test_camera.cpp).

## Instance buffers and CPU/GPU contracts

[Gl.h](../src/render/Gl.h) supplies RAII buffers, VAOs, textures, color targets
and fence rings. Bulk instance buffers use persistent/coherent mapping with
rotating regions and GPU fences. Wait for the region before CPU writes; signal
after draws that read it. Keep GL resources alive only while their context is
current. App tears down GUI/renderer before destroying the window.

The CPU does not memcpy arbitrary SoA spans into a matching shader layout.
[ChaffBatcher.h](../src/render/ChaffBatcher.h) walks live agents and packs
`ChaffInstance`s into family regions, including interpolation, transforms, tint,
feedback flags and optional density contributions. Other passes pack their own
instances. Family enum order is the chaff batch order.

Important layouts are `ChaffInstance` (32 bytes), `EntityInstance` (32 bytes),
and [ParticleInstance](../src/vfx/Particles.h) (48 bytes). A change must update
C++ members/offsets, VAO bindings, and shader attributes together. Packed flag
bytes encode additional family/crowd/feedback information beyond raw sim flags;
read the batcher and shader together instead of interpreting the whole `u32`
as the chaff `u8`.

Family color, silhouette, tempo, wobble, latch throb, hit flash, replication and
burrow/slither looks are pushed into lower-layer tables by game config adapters.
The renderer cannot include game-config structs. `family_color()` is shared by
UI/VFX; keep family identity consistent when changing colors. Visual size also
feeds gameplay collision radii through the enemy adapter. Chaff and swarmers
also carry a spawn `size_scale`; packing uses the same effective lifetime size
as collision. See [SizeJitter.h](../src/sim/SizeJitter.h).

CPU packing tests: [test_render_batch.cpp](../tests/test_render_batch.cpp),
[test_render_lod.cpp](../tests/test_render_lod.cpp). GL integration/species tests:
[test_render_gl.cpp](../tests/test_render_gl.cpp),
[test_render_species.cpp](../tests/test_render_species.cpp).

## Tissue geometry and lane identity

`submit_tissue(mask, sdf, heartbeat_phase, decor)` can draw with only the sim
clearance field. Real loaded levels/editor previews pass optional `TissueDecor`
with flow, lane-owner/type arrays, smooth render SDF and pattern scale.
These inputs are plain arrays/sim references because render does not depend on
`game::LaneOwnershipMap` or `LevelDef`.

The smooth render SDF comes from the shared level geometry bake in
[RenderSdf](../src/game/level/RenderSdf.h), giving swept spline/capsule boundaries
and bounded concave fillets. Its sign contributes to the sim mask at load;
the sim clearance field remains a mask-based EDT. Runtime clot/scar bars alter
walkability/flow while leaving that static clearance/geometry field alone.
Their own entities/effects draw the runtime obstruction.

Decor arrays must outlive draw use. The smooth-SDF texture cache keys include
its backing pointer, so replacing/editing a preview needs the existing dirty/
cache-key changes; do not silently mutate cached geometry and expect a
new upload solely because grid dimensions match. `tissue_pattern_scale()` relates
pattern feature size to reference framing so distant levels retain visible
substrate detail.

Debug flow arrows use `sample_with_support()` so they fade at partial stencil
coverage; squad overlays show authored routes, anchors and group radii. Tests:
[test_render_sdf.cpp](../tests/test_render_sdf.cpp),
[test_render_screenshots.cpp](../tests/test_render_screenshots.cpp).

## Optional density LOD

Density LOD uses local occupancy rather than camera distance. With it enabled,
the batcher samples occupancy bilinearly on cell centers, computes a smooth
crossfade between `lod_blob_threshold` and `lod_blob_full`, and divides each
agent between sprite alpha and blob mass:

```text
sprite_alpha + blob_weight = 1
blob contribution = blob_weight * agent density
```

The low-resolution density texture accumulates premultiplied family color and
mass. `blob.frag` recovers a mass-weighted color. Above the full threshold the
agent needs no sprite instance; in the band it contributes to both paths.
Missing occupancy falls back to ordinary sprites.

Defaults are threshold 24, full 48, density size 320×180, and enabled false.
The current contact model often keeps ordinary cells below the thresholds;
turning the pass on can soften crowded detail into a low-resolution fog. Enable
it only with measured visual/performance evidence for the target scene.
[test_render_lod.cpp](../tests/test_render_lod.cpp) checks complementary weights,
mass/color deposition and batch behavior; it does not establish a full-frame
60 FPS guarantee.

## Fluid surface

`submit_fluid()` uses a distinct two-stage screen-space surface pass. It first
adds particle thickness into an offscreen float target, then thresholds/shades
the continuous field with gradient-derived normals, highlights and depth tint.
Neighboring particles therefore merge into a continuous body instead of a fog
of independent translucent discs.

Pass the solver's `draw_radius()` rather than invent a render radius that leaves
gaps at rest spacing. Resize rebuilds the screen-space thickness target so it
matches the viewport. Use fluid solver tests for gameplay and GL/VFX captures
for the wet surface; surface changes cannot alter the fluid store.

## Combat events and cosmetic particles

[CombatEvents.h](../src/sim/CombatEvents.h) is the one-way instant-event channel.
Sim emits position/source/target/look data before the underlying thing vanishes.
The sink is single-threaded, bounded, drops newest events on overflow and counts
drops. Chaff deaths have an additional per-tick cap so mass retirement does not
consume every muzzle/impact event slot. Parallel producers would need separate
sinks merged in a defined order, not arbitrary atomic append ordering.

[ParticleSystem](../src/vfx/Particles.h) has its own RNG, capacity-bounded SoA,
swap retirement and render-rate `dt`. App drains events, emits effects, clears
the sink, updates particles, then submits additive and alpha instances. Particle
overflow is cosmetic and reported. Gameplay never depends on particle lifetime
or count. Paused/result frames can still animate cosmetics independently of
fixed world ticks.

Kinds include tracer, spark, ring, shard, mist, beam, bolt, cell fragment and
globule. The CPU integration is shared; kind-specific appearance is in the
particle shader. [DeathVfx.h](../src/vfx/DeathVfx.h) supplies family/cell death
look tables, populated from config. The renderer also keeps brief chaff death
bodies so lethal-hit feedback survives chaff compaction; these are renderer
corpses, not living sim agents.

For a new effect, choose a persistent-state draw or an instantaneous event,
include the necessary position/identity before retirement, implement its VFX
consumer/shader, and prove enabling/disabling it does not change sim state.
Tests: [test_particles.cpp](../tests/test_particles.cpp),
[test_chaff_death_vfx.cpp](../tests/test_chaff_death_vfx.cpp),
[test_render_vfx.cpp](../tests/test_render_vfx.cpp).

## Shader iteration and failure handling

[ShaderManager](../src/render/Shader.h) loads text sources relative to
`assets/shaders`. Current graphics programs use `.vert` / `.frag`; they are not
all `.glsl` files. `set_define()` inserts definitions after `#version`.
`poll_reload()` recompiles changed source and retains the working program on a
failed compile/link. Initial shader failure can leave a pass invisible while
Renderer initialization still succeeds; inspect logs/`last_error()` rather than
treat `ready()` as proof every pass compiled.

The API can load compute programs, but no active shipped GPU chaff solver should
be inferred from that capability; the current simulation runs on CPU. Shader
layout edits need both CPU packing and real GL checks. Keep world art in its
established procedural shader path; fonts/icons are separate atlas assets.

## Captures and verification

`--screenshot` builds a world, applies optional setup commands, advances to a
tick, opens a hidden real GL window at the requested size, draws, reads RGBA8,
flips to top-down rows and writes PNG using
[Screenshot.cpp](../src/render/Screenshot.cpp). It needs a GL-capable driver;
hidden does not mean GPU-free.

During offline advancement it drains events and steps particles once per fixed
tick, retaining a short death-event history for correctly aged death bodies.
Normal captures tick the engine without a wave/economy session; `--ui` uses the
session path and player UI model. The capture caller is a separate path from
App: for example, App explicitly submits hostile toxin shots while the current
offline draw list does not. A capture's missing pass can be a caller-wiring issue
even when the interactive shader works.

Pin level/config/seed/workers/tick/setup/focus/view height/resolution and compare
metadata state hashes alongside the image. Cosmetic wall time, GPU/driver and
font rendering can vary independently of gameplay state; a one-frame image is
not evidence of animation or frame-time performance. Use a short sequence for
motion and inspect full-size images for edge/readability changes.

See [TESTING.md](TESTING.md), [GYM.md](GYM.md), and
[test_render_screenshots.cpp](../tests/test_render_screenshots.cpp) for commands
and existing capture cases.
