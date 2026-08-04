# IMMUNE — Game Design Document

**Working title:** IMMUNE
**Genre:** Tower Defense / Mass-Horde Spectacle
**Engine/Language:** Custom C++ (see §8 Technical Architecture)
**Target scale:** 5,000–10,000 concurrent enemy agents, 60 FPS

---

## 1. High Concept

You play as the immune system of a host body. Waves of pathogens surge along the body's own transport network — capillaries, lymphatic vessels, mucosal surfaces — and you defend the host by deploying immune cells that swarm, tag, dissolve, and consume them by the thousand.

It's a lane-based tower defense where **the horde itself is the spectacle**: not "an enemy" with a health bar, but a living, readable river of thousands of pathogens that your defenses visibly carve, redirect, and erode in real time.

**Pillars:**
1. **Flow over individuals** — the player reads and fights a mass, not a unit.
2. **Readability at chaos scale** — always legible which lane is threatened, by what family, how badly, even with 10k sprites on screen.
3. **Biological spectacle** — VFX and mechanics are grounded in (stylized) real immunology, which gives the game a distinct identity and a built-in vocabulary of cool-looking abilities.
4. **Lane-based clarity, sandbox-flavored chaos** — classic TD structure with a fluid-sim feel in execution.

---

## 2. Setting & Narrative Framing

The player operates from inside a host body across a campaign of anatomical "regions," each reskinning the TD lane format:

| Region | Lane character | Design role |
|---|---|---|
| Skin / Epidermis breach | Narrow, tutorial-scale lanes | Onboarding |
| Capillary network | Thin, winding, single-file chokepoints | Precision/chokepoint teaching |
| Lymphatic corridors | Wide, slow flow | Home turf; AoE/DoT teaching |
| Mucosal surfaces (lung, gut lining) | Open floodplains, low lane structure | Horde-control spectacle levels |
| Organ chambers (liver, lymph node, heart valve) | Hub-and-spoke, multiple convergent entries | Boss arenas |

Loose narrative beats (infection escalating from minor cut → systemic threat → autoimmune crisis) justify difficulty ramp and unlock order without requiring heavy story investment. Optional flavor: mission briefings framed as the body's own signaling (fever, inflammation, etc.) reacting to your performance.

---

## 3. Core Gameplay Loop

1. **Scout** the level's vessel layout (lanes, bifurcations, chokepoints).
2. **Deploy** immune cell towers using a resource (see §5 Economy) before/during waves.
3. **Survive waves** — hordes flow along baked paths; player watches the color-mass thin, redirect, or break through.
4. **React** — reposition/upgrade towers, trigger cooldown abilities (Complement Cascade, Histamine Flare) at critical density spikes.
5. **Clear** the wave / defend the objective structure (organ integrity meter) to progress.
6. Between levels: light meta-progression (unlock new cell types, passive "antibody memory" upgrades).

---

## 4. Level & Lane Design

- Lanes are **vessel-shaped paths** baked from level geometry into a **flow field** (see §8.3) rather than discrete tile paths — this lets lanes have organic width, branch, and merge like real vasculature instead of grid corridors.
- **Bifurcations** are the primary "interesting decision" points: mass your defense at a fork, or split coverage.
- **Chokepoints** (capillary pinches) reward single-target/precision towers; **floodplains** (mucosal levels) reward AoE/field towers and are where agent count peaks (the "wow" levels).
- **Organ chambers** are hub arenas: multiple entry vessels converge on a central structure (integrity meter) — closest analog to a "core defense" TD mode, used for boss/finale levels.
- Placement is **grid-free**, continuous positioning along the tissue surface layer, snapped lightly to valid "tissue" placement zones (keeps it approachable without feeling like graph paper).

---

## 5. Towers — Immune Cell Roster

| Cell | Role | Mechanic | Visual identity |
|---|---|---|---|
| **Macrophage** | Melee sink | High single-target DPS/HP, literally "eats" (consumes) elites over time | Chunky, slow, engulfing animation |
| **Neutrophil** | Swarm response | Spawns short-lived micro-units that flood a lane; can drop **NETs** (rooted AoE snare zone) | Fast, numerous, mirrors the enemy horde visually but blue |
| **Dendritic Cell** | Support/utility | No direct damage; marks a horde segment (debuff aura), buffs nearby towers' damage vs marked | Beacon/ping, highlights a horde region |
| **T-Cell (Cytotoxic)** | Precision | High single-target burst, bonus vs elites/bosses | Focused beam, "kiss of death" |
| **B-Cell / Plasma → Antibody** | Tag & chase | Fires semi-autonomous homing antibody projectiles that stick to a target; marked targets take bonus damage from all other sources | Homing projectile with a "stuck" sticker VFX |
| **NK Cell** | Anti-stealth | Detects/punishes hidden or "infected-host-cell" disguised enemies | Sharp, erratic strike pattern |
| **Mast Cell** | Reactive trap | Triggers large AoE "histamine flare" nova when local pathogen density crosses a threshold | Alarm/trigger unit, big juicy nova |
| **Complement Cascade** | Ultimate/support structure | Chain-reaction AoE that jumps pathogen-to-pathogen through dense clusters | Lightning-cascade tearing through the horde |

Each tower has 2–3 upgrade tiers (visual + numeric scaling, no new mechanics mid-tree to keep readability high) and a max of ~1 unique active ability to avoid ability-bloat given how busy the screen already is.

---

## 6. Enemies — Pathogen Families

Enemies are split into **chaff** (cheap, mass-simulated, family-colored, no individual UI) and **named threats** (elites/bosses, individually simulated, telegraphed).

| Family | Color code | Behavior | Chaff/Named |
|---|---|---|---|
| Virus | Red-purple | Fast, weak, **replicates** if not killed quickly (drives exponential horde pressure) | Chaff |
| Bacteria | Yellow-green | Tankier, clumps into biofilm blobs that must be broken apart | Chaff, occasional elite (biofilm colony) |
| Fungal spore | Brown | Drifts rather than runs the lane; leaves lingering hazard zone on death | Chaff |
| Parasite | Teal | Burrows/hides periodically; punishes single-target-only defenses | Named (mid-tier elite) |
| Cancer cell | Sickly grey-pink | Not a horde unit — a slow-growing tumor mass that expands from within a defended zone if ignored | Boss/objective enemy |
| Allergen | Bright yellow (warning color) | Curveball wave type; triggers a friendly "overreaction" risk/reward mechanic (temporary self-damage field) | Special wave modifier |

Design rule: **color = family, silhouette size = threat tier, animation tempo = speed tier.** This trio must remain legible at every zoom level and density the camera supports.

---

## 7. Visual & Art Direction

- **Camera:** topdown, tilted ~15–25°, fixed (no free rotation) for a controlled fake-3D read. Soft directional drop-shadows + slight vertical sprite offset sell height without real 3D geometry.
- **Layers (back to front):** tissue substrate (low-contrast, subtle heartbeat pulse) → lane surface (main action plane) → ambient particulate (drifting cytokines/dust, parallax-scrolls for depth, purely decorative, never occludes gameplay-critical info).
- **Palette:** host tissue = warm, low-saturation pinks/creams (the "floor," must never compete with foreground). Friendly units = cool blue/white/violet. Pathogens = family-coded per §6.
- **Death/impact VFX tiers** (fidelity-budgeted on purpose):
  1. Chaff: dissolve/erosion shader on the color-mass edge where it passes through a damage field — no per-unit death animation.
  2. Elite: individual pop/burst VFX.
  3. Boss: full set-piece animation.
- **Field VFX:** tower AoEs render as literal fluid/chemical fields (toxin clouds, histamine blooms, antibody tides) that visibly reshape the pathogen river — cause and effect must be visible from a zoomed-out view, this is the core spectacle promise.

---

## 8. Technical Architecture (C++)

### 8.1 Engine & core stack
- Custom lightweight engine (not a full commercial engine) — favors full control over the mass-agent renderer/simulation, which is the whole point of the project.
- Suggested base libraries: **SDL2** or **GLFW** for windowing/input, **bgfx** or raw **Vulkan/OpenGL** for rendering, **EnTT** for ECS, **Box2D-lite/custom** broadphase only if needed (likely not — see §8.4, movement is field-based, not physics-based).
- Data-oriented design throughout the hot path (agent update, rendering) — this is a performance-first project, OOP-per-enemy is a non-starter at 10k agents.

### 8.2 Agent tiering (the key architectural decision)
- **Chaff agents (~95% of horde):** no individual game-logic tick. Stored in flat SoA (structure-of-arrays) buffers: position, velocity, family ID, HP-as-density-contribution. Updated in batch on the CPU (SIMD-friendly) or offloaded to a compute shader. Rendered via **GPU instancing** (single draw call per family, per-instance transform + color from a buffer). No individual pathfinding — pure flow-field sampling + local separation (cheap boid-lite jitter, not full boids).
- **Named agents (elites/bosses, ~5% or fewer):** full ECS entities with real components (health, AI state machine, telegraphed attacks, individual collision). Standard game-object treatment.
- Damage to chaff is **aggregate**, not per-unit: damage-field towers define a region + kill-rate; chaff agents inside the region are removed probabilistically/by density-threshold per tick rather than needing individual hit registration. This is both a performance necessity and the source of the "erosion" visual language in §7.

### 8.3 Movement: flow fields, not pathfinding
- Each level bakes a **vector (flow) field** over its walkable tissue area (offline or on level-load), giving every point a "direction toward the objective" vector — classic RTS/horde-game technique (see *Supreme Commander*, *They Are Billions*).
- Tower placement that blocks a lane triggers a **field rebake** (incremental, only affected region, not a full-level recompute) so the horde reroutes visibly and immediately.
- Chaff agents just sample the field at their position each tick + a small separation impulse from a coarse spatial hash (uniform grid, cell size ≈ agent radius × k) to avoid total stacking. No A*, no per-agent pathing — this is what makes 10k agents tractable.
- Named agents can layer additional local steering (dodge telegraphed attacks, burrow, etc.) on top of the same field.

### 8.4 Spatial queries
- Uniform grid spatial hash, rebuilt or incrementally updated per frame, used for: separation impulses, AoE/field-tower queries ("which chaff cells overlap this damage field"), targeting queries for named-agent towers (nearest/strongest in range).
- Avoid per-agent-per-tower collision checks; towers query the grid for cells in range, not agents individually.

### 8.5 Rendering pipeline
- Single instanced draw call per (family × animation-frame-batch) — GPU reads a per-instance buffer (position, rotation, color tint, scale) updated from the CPU-side SoA data each frame (or written directly by a compute shader if movement is GPU-side).
- LOD by density, not distance (camera is fixed-ish, so classic distance LOD doesn't apply): when local agent density in a screen region exceeds a threshold, degrade from per-agent instances to a cheaper "density blob" shader representation seamlessly, and back, so the game never has to draw literally 10,000 unique instanced sprites in a tiny area — it fades to a shader-driven mass representation that looks the same but costs far less.
- Field VFX (toxin clouds, histamine blooms) as screen-space or render-target-based shader effects, not particle-per-agent.

### 8.6 Performance targets & budget (rough, to validate in prototyping)
- 10,000 chaff agents: flow-field sample + separation + instanced render, target < 4ms/frame combined on mid-range hardware.
- ≤ 200 named agents: standard ECS tick, target < 2ms/frame.
- Spatial hash rebuild: amortized/incremental, < 1ms/frame.
- Reserve remainder of frame budget for VFX, UI, audio.

### 8.7 Suggested milestone order for prototyping
1. Flow-field generation + single-family chaff movement at 10k agents, no rendering fidelity (colored quads). **Validate the core performance assumption first.**
2. GPU instanced rendering + density-LOD fallback.
3. One tower (aggregate damage field) + erosion death visual.
4. Named-agent ECS layer + one elite type.
5. Lane/level authoring pipeline (how levels define their tissue mask + spawn/objective points).
6. Full tower roster + enemy roster.
7. UI/economy/wave director.
8. Art/VFX pass, audio, meta-progression.

---

## 9. Economy & Progression (light, TBD detail pass)

- In-level resource (suggest: "ATP" or "signal") earned passively + per-kill, spent on tower deployment/upgrades — standard TD economy, no innovation needed here since the horde/rendering is where the design budget goes.
- Meta-progression between levels: unlock new cell types, passive "antibody memory" modifiers (light roguelite-adjacent loadout choice before a run), cosmetic pathogen/tissue skins per region.

---

## 10. Open Questions

- Single-player campaign only, or an endless/horde-survival mode as a secondary pillar (likely a strong fit given the tech)?
- Co-op (two players, shared vessel network, split lane responsibility)?
- Precise input/targeting scheme for tower placement given the grid-free continuous lane surface — needs a prototype pass.
- Exact aggregate-damage-to-chaff formula (probabilistic removal vs. deterministic density thinning) — needs a feel pass once §8.7 milestone 3 is playable.

---

*This document covers vision, content, and the technical strategy required to hit the 10k-agent target. Numeric balance (damage, costs, wave curves) is intentionally left for a post-prototype tuning pass.*
