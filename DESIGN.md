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
4. **Lane-based clarity, fluid-feel chaos** — classic multi-lane TD structure (discrete, named, biologically distinct lanes you can scout and commit to) executed with a horde that *moves and reacts like a fluid* — it piles up against obstacles, splits around them, and finds the gaps, rather than behaving like a queue of individuals. The clarity is structural (you always know which lane, which family, how bad); the chaos is how the mass inside that structure actually behaves.

---

## 2. Setting & Narrative Framing

The player operates from inside a host body across a campaign of anatomical "regions," each reskinning the multi-lane TD format with its own lane count, vessel character, and fluid behavior emphasis — see the region table in §4.4 for the concrete breakdown (kept in one place so it doesn't drift out of sync with the level-design rules it supports).

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

### 4.1 Lanes are real, named, and constraining — not a mood

A level is built from a small, fixed set of **discrete lanes** — classic-TD-legible, the way *Bloons* or *Kingdom Rush* levels are legible: you can look at a level before the first wave and say "there are three lanes, they converge here, that one's the dangerous one." Each lane is one continuous vessel-shaped corridor from its own spawn point to a shared or private objective. A level typically has **2–4 lanes**; a lane may fork or merge with another *once* as a designed set piece (a bifurcation), but it does not dissolve into an undifferentiated open mesh of splines the way a purely organic vascular network would. **Structure first, organics second** — the biology reskins a legible lane, it doesn't erase the lane.

Each lane carries **hard geometry**: enemies inside it cannot leave its walkable bounds. This is the biological framing for a wall-following TD path — you are not walling off arbitrary grid tiles, you're defending the actual inside of a vessel, and a pathogen physically cannot swim out through the vessel wall. That constraint is what makes the fluid-feel movement in §4.2 read as *contained* pressure — a horde piling up and slamming against a wall inside a capillary is a very different, more anxious picture than the same horde spreading out freely across open ground, and the level design should exploit that difference on purpose (see the region table in §4.4).

**Lane identity, not just lane geometry:** every lane in a level should be a *distinct vessel type* with its own visual tone and (where the level wants it) its own family bias — an artery lane running hot red-orange with fast arterial flow, a lymph lane running pale gold and sluggish, a nerve-adjacent lane running cool violet with erratic pacing. A 3-lane level is not "three copies of the same pipe" — it's three different reasons to look at three different parts of the screen, each telegraphed by color before the player reads a single density number. See §7.2.

### 4.2 The horde behaves like a fluid, not a queue

This is the single most important feel target in the game and deserves to be stated as its own design rule, not buried in a bullet: **the horde must look and behave like it has mass and momentum, not like a line of units taking turns.** Concretely:

- **Pressure and pile-up.** When the front of the horde hits an obstruction — a tower footprint, a chokepoint, a wall — it doesn't politely queue single-file. It **piles up**: density visibly increases at the obstruction, the mass bulges and thickens the way water backs up against a dam, and agents further back keep arriving and compressing into the same crowded space until pressure finds a way through or around.
- **Splash and redirect.** When the piled-up front can't go forward, it goes *sideways along the obstruction* — a visible "splashing" fan-out that hugs the obstacle's silhouette and searches for the gap, exactly like water hitting a rock and curling around both sides before rejoining downstream. This is not a cosmetic flourish layered on top of pathfinding — it *is* the pathfinding, expressed at the density of thousands of agents (see §8.3 for how the same flow-field-plus-local-steering approach that already drives movement produces this for free once tuned for it — no literal fluid solver required, the *feel* is the target, not the physics).
- **Convergence and rejoin.** Past the obstruction, the split mass doesn't stay split forever — it curls back toward the lane's main flow direction and re-merges, the way parted water rejoins downstream of a rock. A tower's kill zone should visibly carve a "wake" into the horde's shape that heals back up a short distance later if the tower doesn't finish the job.
- **Bow waves at chokepoints.** A narrowing lane should produce a visible backpressure bulge upstream of the pinch — the horde looks *compressed*, not just "the lane got thinner." This is one of the primary intended reads of the chokepoint region archetype (§4.4) and should be tuned to be obvious at a glance, not something you have to squint at.

**Design implication:** tower placement is not just "block a tile," it's **applying an obstruction to a fluid system**, and the player should be able to predict, roughly, how the horde will deform around what they place — a tower dropped mid-lane should visibly create a pile-up, a splash-around, and a re-converge, all as one continuous readable event. This is the payoff DESIGN.md's spectacle promise is actually about (§7's "cause and effect must be visible from a zoomed-out view") — the fluid feel is what makes *that specific promise* land, rather than "some agents near the tower disappeared."

### 4.3 Lane geometry as decision space

- **Bifurcations** are the primary "interesting decision" points within a lane: mass your defense at a fork, or split coverage across both branches. Because the horde is fluid-feeling, a bifurcation is also a literal fluid-splitting event — the mass visibly divides in proportion to which branch offers less resistance, so a player who's already thinned one branch will visibly see *more* of the horde routed toward it staying open, versus a heavily-defended branch pushing the flow toward its sibling. That routing behavior should be legible and exploitable, not hidden math.
- **Chokepoints** (capillary pinches) reward single-target/precision towers and are where the bow-wave/pile-up read (§4.2) is most dramatic.
- **Floodplains** (mucosal levels) reward AoE/field towers, have the loosest lane-wall constraint (wide enough that "the wall" barely matters most of the time), and are where agent count peaks — the "wow" levels, and the ones where the fluid feel is most visible simply because there's room for it to happen.
- **Organ chambers** are hub arenas: multiple lanes (not an open mesh — still discrete, still named) converge on a central structure (integrity meter), each from its own entry point around the chamber's perimeter. This is the closest analog to a "core defense" TD mode and the natural home for multi-lane boss/finale levels where the player must split attention across lanes that are *simultaneously* pressuring one shared objective.
- Placement is **grid-free**, continuous positioning along the tissue surface layer, snapped lightly to valid "tissue" placement zones bordering the lane (keeps it approachable without feeling like graph paper) — but placement zones are lane-adjacent, not lane-agnostic: a tower belongs to the lane it borders, and the player's mental model should always be "I am defending *this* lane," even in a level with several.

### 4.4 Region table (unchanged in spirit, reframed around lane count and fluid behavior)

| Region | Lane structure | Fluid behavior emphasis | Design role |
|---|---|---|---|
| Skin / Epidermis breach | 1 lane, narrow, short | Gentle pile-up only — teach that obstacles cause crowding before teaching splash-around | Onboarding |
| Capillary network | 1–2 lanes, thin, winding, single-file chokepoints | Bow-wave/pile-up is the star; splash-around is subtle because the vessel is barely wider than the horde | Precision/chokepoint teaching |
| Lymphatic corridors | 2–3 lanes, wide, slow flow | Splash-and-rejoin is easy to see and slow enough to read calmly | Home turf; AoE/DoT teaching |
| Mucosal surfaces (lung, gut lining) | 2–4 wide lanes, loose walls, low structure | Full fluid spectacle at agent-count peak — pile-up, splash, rejoin, bow waves, all at once | Horde-control spectacle levels |
| Organ chambers (liver, lymph node, heart valve) | 3–5 lanes converging on one hub | Multiple simultaneous fluid fronts pressuring one objective from different angles | Boss arenas |

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

Visual design is not a coat of paint applied after the systems work — every rule below exists because a specific piece of gameplay needs to be *readable*, and the art is the only channel that can carry it at 10,000 agents. If a visual choice doesn't make a mechanic easier to read or react to, it's decoration and should lose to one that does.

### 7.1 Camera & layers

- **Camera:** topdown, tilted ~15–25°, fixed (no free rotation) for a controlled fake-3D read. Soft directional drop-shadows + slight vertical sprite offset sell height without real 3D geometry.
- **Layers (back to front):** tissue substrate (low-contrast, subtle heartbeat pulse) → lane surface (main action plane) → ambient particulate (drifting cytokines/dust, parallax-scrolls for depth, purely decorative, never occludes gameplay-critical info).

### 7.2 Lane identity is a first-class visual system

Per §4.1, every lane in a multi-lane level is a distinct vessel type, and that difference has to be legible from a glance at the whole screen, before the player reads any density or counts any agents:

- Each vessel-type archetype (artery, vein, lymph channel, nerve-adjacent duct, mucosal fold, etc.) gets a **fixed tissue-substrate hue** — warm red-orange for arterial, pale gold for lymphatic, cool violet for nerve-adjacent, and so on — consistent across the whole campaign so a returning player recognizes "that's an artery lane" instantly in a brand-new level.
- The hue lives in the **tissue substrate only** (per §7.1's layering), never in the pathogens themselves — family color-coding (§6) must stay legible regardless of which lane a pathogen is currently in, so lane identity and pathogen identity never compete for the same color channel.
- Lane-specific ambient behavior reinforces the identity passively: arterial lanes pulse faster (heartbeat-linked substrate animation), lymphatic lanes drift slower, matching each vessel type's real-world tempo without requiring the player to read anything — it's felt before it's understood.
- **This is also the answer to per-lane threat readability** (an open question in earlier passes of this document): rather than a separate abstracted HUD icon per lane, the lane *is* the readout — a lane under heavy horde pressure visibly thickens, brightens, and pushes warmer/more saturated relative to its resting hue, so "which lane is in trouble" is answered by the same glance that tells you which lane is which. A dedicated small HUD indicator per lane (icon + dominant-family color, per §6's silhouette/tempo rule) is a legitimate *supplement* once in-fiction readability is solid, but it should never be the primary channel — the mass itself is always the truth.

### 7.3 Palette

Host tissue = warm, low-saturation pinks/creams as the shared baseline "floor," modulated per-lane by §7.2's hue system — the baseline must never compete with foreground regardless of which lane hue is layered over it. Friendly units (towers) = cool blue/white/violet, deliberately the opposite temperature family from every lane hue so towers always read as "not part of the flow." Pathogens = family-coded per §6, and that coding is the one constant that never shifts with lane or region.

### 7.4 The fluid feel is a rendering problem as much as a simulation one

§4.2 defines the horde's *behavior* as fluid — pile-up, splash, rejoin, bow waves. None of that lands if the rendering doesn't sell it:

- **Mass, not sprites, at range.** The density-LOD system (chaff resolves to a shader-driven "mass" representation under crowding, individual instances only where density is low) is exactly the right substrate for this — a pile-up or bow wave should read as the *blob* visibly swelling and deforming, the same way a real fluid surface would, not as "more dots stacked in the same spot." The blob shader is where the fluid feel is actually sold, and it should be treated as gameplay-critical rendering, not a performance fallback that happens to look okay.
- **Leading-edge silhouette is the whole story.** A splash-around only reads if the *outline* of the mass is crisp and legible against the tissue substrate at every density level — the moment the leading edge gets muddy, the fluid illusion collapses into "colored fog." Prioritize edge contrast over interior detail everywhere the horde touches an obstacle.
- **Obstacles get their own visible disturbance response**, not just the horde: a tower footprint or chokepoint wall should show a subtle compression/ripple cue right where the horde is pressing against it — this is what makes the pile-up feel like *pressure against something*, rather than the horde just stopping in place.

### 7.5 Combat visibility — the fluid mass must visibly react to being hurt

- **Field VFX:** tower AoEs render as literal fluid/chemical fields (toxin clouds, histamine blooms, antibody tides) that visibly reshape the pathogen river — cause and effect must be visible from a zoomed-out view, this is the core spectacle promise, and per §4.2 the *specific* effect to sell is a visible wake: the mass thins and deforms as it passes through the field, exactly like the splash-around behavior but caused by damage rather than geometry. A damage field and a wall should look like different *causes* of the same kind of visible disturbance, not unrelated effects.
- **Death/impact VFX tiers** (fidelity-budgeted on purpose):
  1. Chaff: dissolve/erosion shader on the color-mass edge where it passes through a damage field — no per-unit death animation. This is the mass "thinning" described above, continuously, not a discrete event.
  2. Elite: individual pop/burst VFX, plus a clearly telegraphed wind-up beforehand (silhouette/color change or a readable warning shape) so an elite's attack is anticipated, not just suffered — telegraphing an attack that the player can't see coming defeats the point of telegraphing it.
  3. Boss: full set-piece animation.
- **Tower activity must be visible even when nothing is dying.** A tower with no target should still read as "on" (idle animation, a faint standing field if it has one) versus "off" (disabled/unpowered) — the player should never have to guess whether a placed tower is doing anything.

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

### 8.3 Movement: flow fields, not pathfinding — and how this produces §4.2's fluid feel for free

- Each level bakes a **vector (flow) field** over its walkable tissue area (offline or on level-load), giving every point a "direction toward the objective" vector — classic RTS/horde-game technique (see *Supreme Commander*, *They Are Billions*).
- Tower placement that blocks a lane triggers a **field rebake** (incremental, only affected region, not a full-level recompute) so the horde reroutes visibly and immediately.
- Chaff agents just sample the field at their position each tick + a small separation impulse from a coarse spatial hash (uniform grid, cell size ≈ agent radius × k) to avoid total stacking. No A*, no per-agent pathing — this is what makes 10k agents tractable.
- Named agents can layer additional local steering (dodge telegraphed attacks, burrow, etc.) on top of the same field.
- **This is the actual mechanism behind the fluid feel in §4.2, not a separate system layered on top.** Pile-up is what a rebake-invalidated region plus unchanged agent inflow already looks like — agents keep sampling a flow field that no longer has a clear forward path near the obstruction, so they slow and compress there by construction. Splash-around falls out of the same sample once the field's gradient bends around the obstruction toward the nearest still-open route — agents don't need a separate "flow around obstacles" behavior, they need the *baked field itself* to already curve that way, which an SDF-aware bake (distance-to-wall feeding the cost field, not just binary walkable/blocked) gives naturally: cost rises smoothly near a wall rather than jumping at the boundary, so the gradient sweeps agents along the wall face instead of routing them to hug it at zero clearance. Bow-wave pile-up at chokepoints is the same cost-gradient compression, just visible earlier because the whole lane width is narrow. **Practical implication: the fluid feel is a tuning target for the flow-field bake and the separation impulse, not a new subsystem** — the region-table (§4.4) split between "gentle pile-up only" and "full spectacle" tunes are different bake/separation parameters on the same underlying system, not different code paths.

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
- ~~What is a "lane" given organic vessel-spline geometry?~~ Resolved by §4: lanes are discrete, named, and hard-walled — the vessel geometry is styling on a classic lane structure, not a replacement for one.
- **Exact lane count per level and per region** — §4.4 gives ranges (1 lane onboarding up to 3–5 in organ chambers); the precise number per specific level is a content-design decision that needs level-by-level judgment once real levels are being built, not a formula.
- **How strict is "a lane forks or merges at most once"?** (§4.1) Stated as a legibility guardrail against the geometry sprawling into an unreadable mesh — worth stress-testing with an actual 2-fork or hub-with-5-lanes layout before locking it as a hard rule versus a strong default.
- **Tuning the fluid-feel parameters** (§8.3's bake cost gradient + separation impulse) to actually produce a convincing pile-up/splash/rejoin at each region's intended intensity (§4.4) is un-prototyped — this is the single highest-risk "does it actually feel right" item in the whole document and should be validated early, on one chokepoint and one floodplain lane, before content production leans on the feel being correct everywhere.
- **How much does a lane's lymphatic/arterial/nerve identity (§7.2) drive gameplay** versus purely reinforcing lane recognition? An option worth deciding explicitly: lane vessel-type could carry a mild passive modifier (e.g., arterial lanes run faster than lymphatic ones, changing available reaction time) rather than being cosmetic-only — this would make "which lane is which" a tactical read, not just a recognition one, but adds a rule the player has to learn.

---

*This document covers vision, content, and the technical strategy required to hit the 10k-agent target. Numeric balance (damage, costs, wave curves) is intentionally left for a post-prototype tuning pass.*
