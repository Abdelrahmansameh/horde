# IMMUNE — Game Design Document

**Working title:** IMMUNE
**Genre:** Tower Defense / Mass-Horde Spectacle
**Engine/Language:** Custom C++ (see §12 Technical Architecture)
**Target scale:** 5,000–10,000 concurrent enemy agents, 60 FPS

---

## 1. Overview & Positioning

### 1.1 High concept

You play as the immune system of a host body. Waves of pathogens surge along the body's own transport network — capillaries, lymphatic vessels, mucosal surfaces — and you defend the host by deploying immune cells that swarm, tag, dissolve, and consume them by the thousand.

It's a lane-based tower defense where **the horde itself is the spectacle**: not "an enemy" with a health bar, but a living, readable river of thousands of pathogens that your defenses visibly carve, redirect, and erode in real time.

### 1.2 Reference points

The structural and moment-to-moment design backbone is **classic fixed-lane tower defense in the tradition of *Sir, We Have an Orc Problem*, *Bloons TD*, and *Kingdom Rush*** — not a looser, "sandbox" horde game. Concretely, this means:

- **Fixed, discrete, designed lanes**, not an open field or a procedurally-organic mesh. A level is a hand-authored layout you can read at a glance before the first wave.
- **Route bends and intersections are the real strategic geography.** The single biggest lesson pulled directly from *Orc Problem*: good TD play is about **concentrating overlapping firepower at the few points where enemies stay longest in range** (a bend, a switchback, a fork), not spreading towers evenly across the map. IMMUNE's level design should actively manufacture these moments — see §4.3.
- **An in-run economy that rewards upgrading what you have over endlessly expanding.** *Orc Problem*'s stated strategic wisdom — "upgrade existing turrets before adding distant defenses" — is adopted as an explicit economic design goal (§7.1), not left to emerge by accident.
- **A roguelite meta-progression loop layered on top of a level campaign**: failed and successful runs alike feed two persistent currencies that buy permanent, cross-run upgrades in one skill tree ("Strengthen Immunity"). This is adapted directly from *Orc Problem*'s farming loop (§7.2, fully specified in `PROGRESSION.md`) — earlier drafts left meta-progression as a vague TBD.
- **A small number of powerful, cooldown-gated *active abilities*, separate from towers** — screen-changing "break glass in emergency" tools, not another tower to place. Adapted from *Orc Problem*'s three emergency attacks; here reskinned as biological cascade responses (§5.6).
- **Where IMMUNE diverges on purpose:** the horde is not "tens of thousands of interchangeable orcs" rendered as a crowd — it's a *coherent, fluid mass* that visibly deforms, piles up, and splits around obstacles (§4.2), and damage is aggregate/field-based rather than per-unit hit registration (§12.2). That's the game's own identity and the reason the custom renderer/sim exists at all. Borrow the *structure* of classic TD; keep the *spectacle* that's unique to this project.

### 1.3 Pillars

1. **Flow over individuals** — the player reads and fights a mass, not a unit.
2. **Readability at chaos scale** — always legible which lane is threatened, by what family, how badly, even with 10k sprites on screen.
3. **Biological spectacle** — VFX and mechanics are grounded in (stylized) real immunology, which gives the game a distinct identity and a built-in vocabulary of cool-looking abilities.
4. **Lane-based clarity, fluid-feel chaos** — classic multi-lane TD structure (discrete, named, biologically distinct lanes you can scout and commit to) executed with a horde that *moves and reacts like a fluid* — it piles up against obstacles, splits around them, and finds the gaps, rather than behaving like a queue of individuals.
5. **Concentrate, don't sprawl** — the interesting decision is always "where do I stack overlapping defense," not "how do I cover every inch." Level geometry, economy, and UI all exist to make that decision legible and rewarding (§4.3, §7.1).

### 1.4 Target audience & scope

- **Comparable games:** *Sir, We Have an Orc Problem*, *Bloons TD 6*, *Kingdom Rush* for structure and pacing; no other game shares the mass-horde-as-fluid rendering approach, which is the differentiator.
- **Session shape:** a single level plays in roughly **8–20 minutes** depending on region (onboarding levels shorter, organ-chamber finales longer); a full campaign run (see §4.5) is the target "one sitting" unit, with the meta-progression loop (§7.2) supporting shorter drop-in sessions between longer ones.
- **Difficulty philosophy:** approachable to start (classic-TD-legible, low lane count, generous prep time), with real difficulty coming from *later* levels and from **optional replays at higher intensity** (§7.4) rather than from opacity. A player should never lose because they couldn't tell what was happening — only because they made a placement or economy decision that didn't hold up.
- **Platform assumption:** mouse/keyboard-first PC (per the existing input model, §5.4/§8.3), designed so a controller or touch adaptation remains plausible later without a redesign (large touch targets in the build menu, no reliance on hover-only information).

---

## 2. Setting & Narrative Framing

The player operates from inside a host body across a campaign of anatomical "regions," each reskinning the multi-lane TD format with its own lane count, vessel character, and fluid-behavior emphasis — see the region table in §4.6.

Loose narrative beats (infection escalating from minor cut → systemic threat → autoimmune crisis) justify difficulty ramp and unlock order without requiring heavy story investment. Mission briefings are framed as the body's own signaling (fever, inflammation, localized swelling) reacting to the player's performance — a light, readable narrator voice rather than a plot the player has to track. Each region gets one short framing beat on first entry (a sentence or two, delivered as the body "noticing" the threat) and nothing more; the fiction exists to make regions and enemies feel distinct, not to carry story weight the gameplay isn't built to support.

---

## 3. Core Gameplay Loop

### 3.1 Macro loop (campaign level)

1. From a **region map** (a simple node graph of levels, unlocked in order with light branching — see §4.5), select a level.
2. Optionally spend Memory Cells / Antibodies (§7.2) on permanent upgrades in the **Strengthen Immunity** tree before entering — there is no separate pre-run loadout step (§7.3); every purchase is already active.
3. Play the level (§3.2).
4. On completion (or failure — §3.3), earn ATP-equivalent meta-currency proportional to performance, return to the region map, repeat.

### 3.2 Per-level loop (moment to moment)

1. **Scout** the level's lane layout during a generous initial prep phase (bends, forks, convergences, and the region's fluid-behavior tells per §4.2) — no enemies on screen yet, full freedom to plan initial placements.
2. **Deploy** immune-cell towers using ATP (§7.1), concentrating at the bends/convergences the level's geometry has manufactured (§4.3) rather than spreading evenly.
3. **Survive waves** — hordes flow along fixed lanes; the player watches the color-mass pile up, splash around obstructions, thin, or break through (§4.2).
4. **React** — upgrade towers already placed (usually the stronger economic move, §7.1), reposition/add new towers where the wave composition demands it, and spend cooldown-gated active abilities (§5.6) at critical density spikes.
5. **Clear** each wave, using the between-wave prep window to re-plan before the next; **defend the objective structure** (organ integrity meter, §4.5) across all of a level's waves to win.
6. A **wave preview panel** (§8) always shows what's coming next — family composition, approximate count, and whether a named elite/boss is inbound — so reactive decisions in step 4 are informed, not guesses.

### 3.3 Failure is not a dead end

Per §1.2's roguelite-loop adoption, **losing a level is not a wasted attempt.** Objective integrity hitting zero ends the run and returns to the region map, but meta-currency earned during that attempt (waves cleared, pathogens killed, elites defeated) is kept and spent on permanent upgrades regardless of outcome. This is a deliberate difficulty-softening design: a level that's currently too hard becomes easier not by the player looking up a strategy, but by attempting it, failing, and coming back permanently stronger — the same farm-then-push loop *Orc Problem* uses. See §7.2 for the exact currency/upgrade structure this funds.

---

## 4. Level, Lane & Wave Design

### 4.1 Lanes are real, named, and constraining — not a mood

A level is built from a small, fixed set of **discrete lanes** — classic-TD-legible, the way *Bloons* or *Kingdom Rush* levels are legible: you can look at a level before the first wave and say "there are three lanes, they converge here, that one's the dangerous one." Each lane is one continuous vessel-shaped corridor from its own spawn point to a shared or private objective. A level typically has **2–4 lanes**; a lane may fork or merge with another *once* as a designed set piece (a bifurcation), but it does not dissolve into an undifferentiated open mesh of splines the way a purely organic vascular network would. **Structure first, organics second** — the biology reskins a legible lane, it doesn't erase the lane.

Each lane carries **hard geometry**: enemies inside it cannot leave its walkable bounds. This is the biological framing for a wall-following TD path — you are not walling off arbitrary grid tiles, you're defending the actual inside of a vessel, and a pathogen physically cannot swim out through the vessel wall. That constraint is what makes the fluid-feel movement in §4.2 read as *contained* pressure, and it's also what makes §4.3's concentrated-kill-zone geometry possible to author on purpose.

**Lane identity, not just lane geometry:** every lane in a level should be a *distinct vessel type* with its own visual tone and (where the level wants it) its own family bias — an artery lane running hot red-orange with fast arterial flow, a lymph lane running pale gold and sluggish, a nerve-adjacent lane running cool violet with erratic pacing. A 3-lane level is not "three copies of the same pipe" — it's three different reasons to look at three different parts of the screen, each telegraphed by color before the player reads a single density number. See §9.2.

### 4.2 The horde behaves like a fluid, not a queue

This is the single most important feel target in the game and deserves to be stated as its own design rule, not buried in a bullet: **the horde must look and behave like it has mass and momentum, not like a line of units taking turns.** Concretely:

- **Pressure and pile-up.** When the front of the horde hits an obstruction — a Fibrin Clot, a NET dam, a wall — it doesn't politely queue single-file. It **piles up**: density visibly increases at the obstruction, the mass bulges and thickens the way water backs up against a dam, and agents further back keep arriving and compressing into the same crowded space until pressure finds a way through or around.
- **Splash and redirect.** When the piled-up front can't go forward, it goes *sideways along the obstruction* — a visible "splashing" fan-out that hugs the obstacle's silhouette and searches for the gap, exactly like water hitting a rock and curling around both sides before rejoining downstream. This is not a cosmetic flourish layered on top of pathfinding — it *is* the pathfinding, expressed at the density of thousands of agents (see §12.3 for how the same flow-field-plus-local-steering approach that already drives movement produces this for free once tuned for it — no literal fluid solver required, the *feel* is the target, not the physics).
- **Convergence and rejoin.** Past the obstruction, the split mass doesn't stay split forever — it curls back toward the lane's main flow direction and re-merges, the way parted water rejoins downstream of a rock. A tower's kill zone should visibly carve a "wake" into the horde's shape that heals back up a short distance later if the tower doesn't finish the job.
- **Bow waves at obstructions.** Anything the flow cannot pass cleanly — a Fibrin Clot, a NET dam, the outer wall of a switchback the mass takes too fast — should produce a visible backpressure bulge *upstream* of it: the horde looks **compressed**, not just "slowed down," and it should be obvious at a glance rather than something you squint at. Note what this bullet no longer says: the bow wave is **something the player manufactures, not something the map hands them.** Levels do not author single-file pinches (§4.3), so if you want this read, you build the thing that causes it.

**Design implication:** placing an obstruction (a Fibrin Clot, a NET) is not just "block a tile," it's **applying an obstruction to a fluid system**, and the player should be able to predict, roughly, how the horde will deform around what they place — a clot dropped mid-lane should visibly create a pile-up, a splash-around, and a re-converge, all as one continuous readable event. Towers themselves are **not** obstructions: the horde flows straight through a tower's footprint and the tower is drawn on top of it, so a tower never reshapes the river — only the things it fires and the abilities the player drops do. This is the payoff the spectacle promise (§9.5) is actually about — the fluid feel is what makes *that specific promise* land, rather than "some agents near the tower disappeared."

### 4.3 Concentrated kill zones — the real strategic geography

The single most important structural lesson adapted from *Orc Problem* (§1.2): **the interesting part of a lane is not its length, it's the handful of points where an enemy stays in range the longest.** A straight corridor is a bad lane — every tower on it sees an enemy for the same short window and there's no decision to make. Level geometry should *manufacture* high-value real estate on purpose:

- **Bends and switchbacks.** A lane that curls back on itself (a hairpin, an S-curve) lets a single, well-placed cluster of towers keep an enemy in range across *multiple* segments of the curl — this is the single highest-value placement pattern in the game and every level should have at least one deliberately generous example of it.
- **Intersections and convergence points** (where two lanes merge, or where multiple lanes approach one organ-chamber objective from different angles) are natural concentration points precisely because pressure from more than one direction arrives at the same spot — towers here do double duty.
- **Player-manufactured obstructions** are the third concentration mechanism, and the only one that isn't geometry: a NET dam or a mid-lane Fibrin Clot (§5.5) compresses the horde *where the player chose to spend*, which is how the game delivers §4.2's bow-wave density without any level authoring a pinch for it.
- **Levels do not ship chokepoints — this is a hard authoring rule, not a preference.** A lane that squeezes down to single-file hands the player the answer: two towers on the slot beat every other option, on every attempt, forever. That is exactly the "every tower sees the same short window and there's no decision to make" failure this section opens by rejecting — a pinch doesn't fix a boring straight, it just moves the boredom from "nowhere is good" to "only one place is good." Concentration must be *found* (a bend, a convergence) or *bought* (an obstruction), never handed over by a narrow corridor. Vessels may taper gently as anatomy; they may not neck down into a kill slot.
- **This is a placement-zone authoring rule, not just a fluid-physics side effect:** when building a level, place generous buildable tissue margins (§4.7) around bends/switchbacks/convergences specifically, and comparatively tight or absent buildable margin along long straight stretches, so the level's *geometry itself* nudges the player toward concentrating defense rather than sprawling it — the map should make the good decision the visually obvious one.
- **Corollary for the economy (§7.1, §5.3):** because concentrated positions are both scarce and high-value, they're also where **combining tower types beats spreading them thin** most clearly — a permanently-strong pair (a slow field feeding a damage-dealer's capstone, say) at a hairpin bend outperforms the same towers split across a straight, and level placement-zone authoring should make that true in practice, not just in theory.

### 4.4 Lane geometry as decision space

- **Bifurcations** are the primary "interesting decision" points within a lane: mass your defense at a fork, or split coverage across both branches. Because the horde is fluid-feeling, a bifurcation is also a literal fluid-splitting event — the mass visibly divides in proportion to which branch offers less resistance, so a player who's already thinned one branch will visibly see *more* of the horde routed toward it staying open, versus a heavily-defended branch pushing the flow toward its sibling. That routing behavior should be legible and exploitable, not hidden math.
- **Switchbacks** (capillary serpentines) reward single-target/precision towers: a cluster placed on the tissue *between* two passes of the same lane gets a second look at everything it failed to kill on the first pass. This is the payoff a pinch used to buy for free — except here the player still has to spot the spot and afford it, which is the entire difference.
- **Floodplains** (mucosal levels) reward AoE/field towers, have the loosest lane-wall constraint (wide enough that "the wall" barely matters most of the time), and are where agent count peaks — the "wow" levels, and the ones where the fluid feel is most visible simply because there's room for it to happen.
- **Organ chambers** are hub arenas: multiple lanes (not an open mesh — still discrete, still named) converge on a central structure (integrity meter), each from its own entry point around the chamber's perimeter. This is the closest analog to a "core defense" TD mode and the natural home for multi-lane boss/finale levels where the player must split attention across lanes that are *simultaneously* pressuring one shared objective.

### 4.5 Level structure & pacing

- **Numbering & sub-stages.** Levels are numbered sequentially within a region (e.g. Capillary 1, 2, 3); a region's final level may have one or two **sub-stage variants** (e.g. "3b: Infected") unlocked by clearing the main level at a bonus objective (see grading, §7.4) — harder remixes of the same geometry with a tougher wave table, not new content to author from scratch.
- **Waves per level:** onboarding levels run **4–6 waves**; standard levels run **8–12 waves**; region-finale organ-chamber levels run **12–18 waves** culminating in a boss encounter (§6.4). This scales session length roughly with §1.4's target range.
- **Prep time.** A generous first-wave prep window (enough to place 2–4 starter towers without rushing) at the top of every level; subsequent inter-wave prep windows shrink slightly as the level progresses, pushing the player from "plan calmly" toward "react decisively" as waves escalate — mechanically, this is the same `WaveDef::prep_time` field already in the technical design, just with an explicit per-level *curve* (long → progressively shorter) rather than a flat value.
- **The escalation curve within one level:** each wave should be reliably tougher than the last along at least one axis (count, speed, a new family introduced, an elite appearing) but not every axis at once — a wave that's simply "the last wave with bigger numbers" reads as escalation without decision-making. Vary *which* axis spikes wave to wave.
- **Difficulty spikes are a deliberate tool, not an accident.** At least one wave partway through a standard-or-longer level should be a visible step up — a sudden elite pairing, a family the player hasn't handled at this density before, or two lanes spiking simultaneously — explicitly telegraphed one wave ahead via the preview panel (§3.2 step 6) so it reads as "the game warned me and I under-prepared," never as an ambush.
- **The final wave is always the hardest wave of the level**, and organ-chamber finales end their final wave with a boss (§6.4), not just a density peak.
- **Endless/overtime option.** Once a level's scripted wave table is cleared, offer an optional **"keep going" endless extension** on the same geometry (procedurally continuing the escalation curve) for players chasing a better grade (§7.4) or simply more of that level's spectacle — this reuses the existing wave-generation approach rather than requiring hand-authored infinite content.

### 4.6 Region table

| Region | Lane structure | Fluid behavior emphasis | Design role |
|---|---|---|---|
| Skin / Epidermis breach | 1 lane, narrow, short | Gentle pile-up only — teach that obstacles cause crowding before teaching splash-around | Onboarding |
| Capillary network | 1–2 lanes, thin, winding, tight switchbacks | Pile-up against player-placed obstructions is the star; splash-around is subtle because the vessel is barely wider than the horde | Precision/bend-reading teaching |
| Lymphatic corridors | 2–3 lanes, wide, slow flow | Splash-and-rejoin is easy to see and slow enough to read calmly | Home turf; AoE/DoT teaching |
| Mucosal surfaces (lung, gut lining) | 2–4 wide lanes, loose walls, low structure | Full fluid spectacle at agent-count peak — pile-up, splash, rejoin, bow waves, all at once | Horde-control spectacle levels |
| Organ chambers (liver, lymph node, heart valve) | 3–5 lanes converging on one hub | Multiple simultaneous fluid fronts pressuring one objective from different angles | Boss arenas / region finales |

The region is a **design brief, not a runtime input**. Each level authors its own explicit wave table in its own JSON file (`assets/levels/*.json`, schema in `game/level/Level.h`); nothing derives a wave table from the region name. Two levels in the same region are expected to differ, and the per-region feel above is achieved by authoring them that way rather than by a shared curve.

### 4.7 Placement rules

- Placement is **grid-free**, continuous positioning along the tissue surface layer, snapped lightly to valid "tissue" placement zones bordering the lane (keeps it approachable without feeling like graph paper) — but placement zones are lane-adjacent, not lane-agnostic: a tower belongs to the lane it borders, and the player's mental model should always be "I am defending *this* lane," even in a level with several.
- **Buildable margin width is a level-design lever, not a fixed constant** (§4.3): generous at bends/switchbacks/convergences, tight or absent along long straights, actively discouraging sprawl.
- **Minimum tower spacing** exists (towers cannot fully overlap footprints) but is deliberately small relative to most towers' range — the goal is to *permit* the concentrated stacking §4.3 wants, not to artificially prevent it the way a strict grid would.
- **No hard per-level tower cap** — the economy (§7.1) is the real limiter, so a maxed-economy late-level attempt can legitimately field a dense cluster without hitting an arbitrary ceiling.
- A tower does **not** block ground. It never edits the tissue mask or the flow field; the horde walks straight through its footprint and the tower renders over the enemies inside it, so a mid-lane tower can never seal a lane and placement needs no reachability check. §4.2's player-manufactured obstructions are the NET and the Fibrin Clot, and it is *their* placement that refuses to fully seal a lane (a soft-lock is never an acceptable outcome of a legal placement).

---

## 5. Towers — Immune Cell Roster

### 5.1 Role taxonomy

Every tower is classified along two independent axes so the roster reads as a coherent kit, not eight unrelated gadgets:

**By fluid role** (how it interacts with the horde-as-fluid, §4.2) —
- **Dam:** primarily reshapes flow — blocks, slows, or redirects, changing *where* the horde goes more than how fast it dies.
- **Erosion:** primarily thins the mass directly — a sustained field that carves the horde down over time.
- **Catalyst:** doesn't damage on its own, but changes how other sources interact with the horde (marking, debuffing, priming a combo).
- **Precision:** ignores the mass entirely and focuses on named threats.

**By functional role** (classic TD archetype, for build-menu grouping and at-a-glance kit legibility) —
- **AoE/coverage**, **rapid/focused single-target**, **crowd-control/support**, **precision/utility** — the same four buckets that fall out naturally when any classic-TD roster is sorted by what it's *for*, used here as a build-menu organizing principle rather than a power tier.

| Cell | Fluid role | Functional role |
|---|---|---|
| Macrophage | Erosion | Rapid/focused (vs. elites) |
| Neutrophil | Dam + Erosion | AoE/coverage |
| Dendritic Cell | Catalyst | Crowd-control/support |
| T-Cell (Cytotoxic) | Precision | Rapid/focused |
| B-Cell / Antibody | Catalyst + Precision | Crowd-control/support |
| NK Cell | Precision | Precision/utility |
| Mast Cell | Erosion | AoE/coverage |
| Complement Cascade | Erosion | AoE/coverage (ultimate) |

### 5.2 Full roster

> **Current roster (supersedes the table below for what ships).** The game
> runs a swarmer roster in which every placed tower is only a factory. During a
> round, towers continuously release small cells that pick a pathogen inside
> their aggro radius and do the tower's work. A released Macrophage grows up
> to three independent branching pseudopods in any direction. Each tree
> extends toward its own target, closes fine terminal fingers around it, and
> pulls the captive into the deforming body, where it dies; the arm then
> recovers and the slot is free to grow again. A tower's live units all stand
> as one LANE WALL: a body-to-body rank across the flow that shoves the horde
> back instead of yielding to it. Cadence, reach,
> drain, movement speed, and the shared swarmer chassis are tuned per tower
> and tier in `assets/config/towers.json`.
>
> | Cell | Swarmer kind | On contact |
> |---|---|---|
> | **Cytotoxic T** | Latch | latches onto the pathogen and drains it; moves on when it dies |
> | **Neutrophil** | Shooter | holds a standoff and fires real rounds; chases if the target leaves |
> | **Macrophage** | Arbor grabber | up to three branching pseudopods extend, latch a target each, and pull them into the body to kill them; the unit's rank forms a wall across the lane |
> | **Interferon** | Slow bomber | detonates into a timed circle on the ground that slows what walks through it (the slow lingers a little after leaving) |
> | **Goblet Cell** | Mucus bomber | detonates into a splash of real mucus that strongly slows what it soaks without dealing damage |
> | **Fibroblast** | Builder | walks a single builder out to lay a collagen scar across the lane; once it's at its cap of standing scars, the builder reinforces one instead of laying a new one |
>
> Swarmer bombers that run out of lifetime detonate where they stand. Shooters
> and bombers stand their ground; latchers always chase. Neutrophil shooters
> released together fight as a squad, in a rank across their approach.
> All swarmers, including Macrophage units, can target named agents as well as chaff;
> burrowed enemies are invisible to all of them. Towers have no active
> abilities of their own — see §5.6 for the four player-triggered abilities,
> which are unlocked separately through meta-progression (§7.2) and are not
> tied to any one tower.
> The NK Cell is retired. The table that follows is the original eight-cell
> design and is kept for the fiction and the role vocabulary.

| Cell | Role | Mechanic | Visual identity |
|---|---|---|---|
| **Macrophage** | Melee sink (Erosion) | Engulfs a small cluster at close range; captives die inside the cell | Chunky, slow, exaggerated pincer-and-swallow animation |
| **Neutrophil** | Swarm response (Dam + Erosion) | Spawns short-lived micro-units that flood a lane; can drop **NETs** — a rooted AoE snare zone that is itself a physical obstruction (a NET is a Dam in the fluid sense, not just a debuff — see §5.5) | Fast, numerous, mirrors the enemy horde visually but blue |
| **Dendritic Cell** | Support/utility (Catalyst) | No direct damage; marks a horde segment (debuff aura), buffs nearby towers' damage vs marked | Beacon/ping, highlights a horde region |
| **T-Cell (Cytotoxic)** | Precision | High single-target burst, bonus vs elites/bosses | Focused beam, "kiss of death" |
| **B-Cell / Plasma → Antibody** | Tag & chase (Catalyst + Precision) | Fires semi-autonomous homing antibody projectiles that stick to a target; marked targets take bonus damage from all other sources | Homing projectile with a "stuck" sticker VFX |
| **NK Cell** | Anti-stealth (Precision) | Detects/punishes hidden or "infected-host-cell" disguised enemies | Sharp, erratic strike pattern |
| **Mast Cell** | Reactive trap (Erosion) | Triggers large AoE "histamine flare" nova when local pathogen density crosses a threshold | Alarm/trigger unit, big juicy nova |
| **Complement Cascade** | Ultimate/support structure (Erosion) | Chain-reaction AoE that jumps pathogen-to-pathogen through dense clusters | Lightning-cascade tearing through the horde |

### 5.3 Tower power is permanent, not in-run

Towers no longer have in-run upgrade tiers. Each tower is placed at a single baseline and every stat above that baseline — damage, rate, range, health, count, everything — comes from whatever the player has permanently purchased in the **Strengthen Immunity** meta-progression tree (§7.2; full design in `PROGRESSION.md`). Every copy of a tower type placed this run, in any level, carries the same permanent bonuses; ATP spent in a level buys placement only (§7.1), there is nothing left to upgrade mid-run.

This retires §1.2's original *Orc Problem*-derived "upgrade over expand" cost curve, and replaces it with a different expression of the same pillar (§1.3 pillar 5, "concentrate, don't sprawl"): since a single tower is already as strong as the player's permanent investment, the interesting decision at a concentrated position (§4.3) becomes **which tower types to combine** — stacking damage, slow, and marking roles together for the cross-tower payoffs the tree's capstone nodes reward (`PROGRESSION.md`) — rather than which one tower to keep feeding tiers into. Sell/refund (§7.1) remains the mistake-forgiveness valve for a bad placement choice.

### 5.4 Targeting rules

- **Chaff-affecting towers** (Erosion/Dam role) never target individual chaff agents — they define a field/region and the aggregate damage system (§12.2) resolves who's inside it. There is no "nearest chaff" targeting concept; the tower's *position and shape* is the targeting decision, made once at placement.
- **Named-agent-affecting towers** (Precision role) use conventional TD targeting rules against the small named-agent population: **nearest**, **strongest** (highest remaining HP), **first** (closest to the objective), and **marked-priority** (prefer a target another tower has already marked, rewarding the Dendritic/B-Cell combo in §5.5) are all valid selectable modes, exposed as a simple per-tower toggle in the info panel (§8) rather than forced to one default — a small but real piece of player agency classic TD players expect.

### 5.5 Synergies

- **Marking is the central combo currency.** Dendritic Cell and B-Cell both apply a "marked" state (to a chaff segment or a named target respectively); *every* other tower deals bonus damage to marked targets. This rewards a specific, legible strategy — commit a support tower, then stack damage towers behind it — rather than every tower being independently optimal.
- **NETs are a Dam, not just a slow.** A Neutrophil's NET zone is a physical obstruction in the fluid-feel movement system (§4.2) — chaff routes *around* it the same way it routes around a wall, meaning a well-placed NET can manufacture its own miniature pile-up/splash event mid-lane, which a Mast Cell or Complement Cascade can then exploit (both explicitly reward *density*). This is the clearest example of a tower interacting with the fluid system itself rather than just with enemy HP, and it's a combo worth calling out explicitly in tooltips/tutorialization (§11).
- **Mast Cell rewards letting the horde bunch up** — a deliberate tension against the instinct to thin every lane immediately, and a natural partner for a NET, whose bow-wave compression it feeds on directly.

### 5.6 Active abilities (separate from towers)

Adapted directly from §1.2's *Orc Problem* reference: a small set of **cooldown-gated, player-triggered, screen-changing abilities**, distinct from towers (no placement, no targeting — a single button press affects the whole visible battlefield or a large targeted radius), meant as emergency tools for the moments a pure tower-and-economy response isn't enough. All four are locked at campaign start and unlocked permanently through the Strengthen Immunity tree (§7.2) — a new game has zero active abilities available, exactly like it has only one tower; see `PROGRESSION.md`.

| Ability | Effect | Biological framing | Risk/reward |
|---|---|---|---|
| **Complement Cascade Burst** | A single massive chain-reaction AoE across the densest visible cluster, on a long cooldown | The complement system's amplification cascade, triggered system-wide instead of locally | Pure upside, but long cooldown means using it early on a small wave wastes it on a boss wave later |
| **Histamine Flare** | A large, brief AoE nova centered on the player's chosen point | Mast-cell-style degranulation at a scale no single tower can reach | High single-moment payoff, short duration — timing matters more than placement |
| **Fever Response** | A temporary global buff (attack speed/damage) to every placed tower for a short window, body-wide | Systemic fever raising immune activity everywhere at once | No direct damage of its own — a force-multiplier that does nothing if towers aren't already well-placed |
| **Fibrin Clot** | Drops a fixed-size bar across the lane at the chosen point for a few seconds; the horde has to squeeze past it, and the flow field reroutes around it as it would around a wall. Refuses a point that would seal the lane outright | Coagulation: a fibrin mesh with platelets caught in it, plugging part of the vessel | No damage at all — buys the towers on either side a longer look at a chokepoint, and does nothing on a lane nothing is defending |

These four exist for the same reason *Orc Problem*'s do: they give the player a way to survive a moment that outpaces their current build without bypassing the core placement/economy loop — they amplify or rescue an existing defense, they don't substitute for one. All four are on independent, meaningfully long cooldowns (multiple waves, not multiple seconds) so choosing *when* to use one is itself a real decision, not a spammable button.

---

## 6. Enemies — Pathogen Families

### 6.1 Chaff vs. named

Enemies are split into **chaff** (cheap, mass-simulated, family-colored, no individual UI) and **named threats** (elites/bosses, individually simulated, telegraphed).

### 6.2 Pathogen families

| Family | Color code | Behavior | Chaff/Named |
|---|---|---|---|
| Virus | Red-purple | Fast, weak, **replicates** if not killed quickly (drives exponential horde pressure) | Chaff |
| Bacteria | Yellow-green | Tankier and slower than a virus; no special behavior — a plain mass unit that reads as raw HP pressure | Chaff |

Design rule: **color = family, silhouette size = threat tier, animation tempo = speed tier.** This trio must remain legible at every zoom level and density the camera supports.

### 6.3 Elite framework

An elite is not "a chaff unit with more HP" — each elite exists to teach or punish a specific defensive gap, and the roster should be sized so every gap the tower kit can leave has at least one elite that tests it.

The elite roster is currently empty (§14). Each elite added to it should be scoped the same way — name the gap first, build the elite to test it second.

Every elite is **individually simulated and telegraphed** (a readable wind-up before its signature attack, per Pillar 2) — the player should always get a fair warning beat before an elite does something a chaff unit couldn't.

### 6.4 Boss / organ-chamber encounter design

A boss is not simply the largest elite — an organ-chamber finale boss should combine **at least two simultaneous pressures** so the encounter can't be solved by one dominant tower:

- A body-blocking or lane-altering presence (a boss that itself acts as a moving obstruction, interacting with the fluid system per §4.2 — the horde piles up *behind the boss* as it advances, a unique visual the game shouldn't waste on a one-off encounter type).
- A periodic telegraphed area attack that punishes over-concentration right where the player is strongest (a direct counter-pressure against §4.3's core strategy, forcing a moment of redistribution).
- Continued chaff/elite pressure from the chamber's other lanes throughout the fight, so the boss is never fought in isolation from the horde-management game the rest of the level trained.

This is a framework for authoring future bosses, not a specific boss kit — see §14 for the open question on exactly how many boss archetypes the campaign needs.

### 6.5 Difficulty scaling across the campaign

- **Within a region:** enemy family mix stays roughly fixed; count, density, and elite frequency escalate level to level (§4.5's escalation curve).
- **Across regions:** each new region should introduce at minimum one new family or elite (per §4.6's design-role column) so difficulty growth is legible as "a new thing to learn," not just "the same thing, more of it." A region that only escalates existing families' numbers is a missed teaching opportunity.
- **Endless/overtime extensions** (§4.5) scale procedurally past a level's authored content using the same family mix, escalating count/speed only — appropriate for a score-chasing extension, not for introducing new mechanics unsupervised.

---

## 7. Economy & Progression

### 7.1 In-run economy (ATP)

- **Resource:** ATP, earned **passively** (a steady trickle over time, funding a baseline build tempo even with no kills) **and per-kill** (proportional to density destroyed, rewarding active engagement over turtling), spent on tower **placement only** — there is no in-run upgrade purchase; a tower's power is whatever the player's permanent meta-progression (§7.2) has already bought into that tower type.
- **Starting ATP and both income rates are global, not per-level.** A single baseline, raised permanently by the Strengthen Immunity tree's economy nodes (§7.2), applies on every level campaign-wide — a level's difficulty comes entirely from its wave design, never from a bigger starting wallet.
- **Concentration is still the core decision (§4.3), just expressed differently** — see §5.3: since towers no longer have tiers to sink ATP into, the efficient move at a concentrated position is combining tower *types* for overlapping/synergistic coverage, and spending ATP is otherwise a straightforward "can I afford to open this position now" call.
- **Sell/refund** exists (partial refund on sale) as a mistake-forgiveness valve, not a strategy — refund fraction should be low enough that repositioning is a real cost, high enough that a genuinely bad placement isn't a run-ending mistake.
- **Elite/boss kills grant a flat bounty** on top of density-proportional income, making named-threat kills feel like a distinct, celebrated economic event rather than "the same income, delivered in one lump."

### 7.2 Meta-progression: Strengthen Immunity

Adapted from §1.2's reference, extended past it: **two persistent currencies**, both earned from **every** run — cleared or failed — fund a single permanent, cross-run skill tree called **Strengthen Immunity**, returned to after every match. This supersedes the single-currency structure of earlier drafts of this section; the full design (currency rules, tree shape, every tower's upgrade lines, the hub branch, and the campaign-pacing rationale) is its own companion document, `PROGRESSION.md` — this section is a summary, not the authority.

- **Memory Cells** — earned from every run's performance (waves cleared, density killed, elites/bosses defeated), never gated by winning, so a failed attempt against a level that's currently too hard still meaningfully funds the next attempt (§3.3's failure-is-not-a-dead-end design). Spent on the tree's incremental stat nodes: every upgradeable number on every tower, plus the hub branch's global economy and active-ability lines.
- **Antibodies** — earned once, on a level's **first** clear only. Spent on the tree's milestone nodes: unlocking each of the five towers the player doesn't start with (freely orderable — nothing in the roster gates behind anything else, per your call on tower order), each tower branch's capstone, and each of the four active abilities' unlocks (§5.6).
- **The loop this creates:** attempt a level → whether you clear it or not, Memory Cells make you permanently a little stronger → clearing it for the first time also grants an Antibody, spendable on a real capability jump (a new tower, a capstone, an ability) → push into new content, or replay an earlier level for more Memory Cells and a better grade (§7.4). This is the primary retention/pacing structure outside the scripted campaign, and it's tuned so a player who's genuinely stuck on one level always has "go farm/replay elsewhere and come back stronger" as a legitimate, non-punishing path forward.
- The game starts with only the **Neutrophil** tower and no active abilities unlocked; everything else — the other five towers, all four abilities, every stat line, every capstone — is purchased from the tree. See `PROGRESSION.md` for the complete node-by-node design.

### 7.3 No per-run loadout — every unlock is permanent

Earlier drafts of this section specified a separate, roguelite-style pre-run "loadout" pick layered on top of permanent upgrades. That system is cut: it overlapped almost entirely with what the Strengthen Immunity tree (§7.2) already does, and a second, temporary layer of the same kind of choice added complexity without adding a distinct decision. Every meta-progression purchase — a stat node, a tower unlock, a capstone, an ability — is permanent and always active from the moment it's bought, on every subsequent run, with no re-selection step. Run-to-run *variance* (drafted relics, biased seeds, and similar roguelite texture) remains a plausible future addition, but as its own separate system layered on top of the permanent tree, not a revival of this one — see `PROGRESSION.md`'s open questions.

### 7.4 Replayability & grading

- Every level awards a **grade** on completion (a simple 1–3 star or equivalent scale) based on performance signals that reward the game's actual skill expression: objective integrity remaining, whether the optional endless extension (§4.5) was engaged, elite/boss kills, and (lightly) speed — never punishing careful, defensive play in favor of pure speed.
- A top grade on a level's main content is the unlock condition for that region's harder sub-stage variant (§4.5).
- **Grade affects Memory Cells only, never Antibodies** — a level's Antibody is a flat, one-time reward for its first clear (§7.2), so replay value stays about tree-funding and bragging rights, not about re-farming a scarce currency.
- Replaying an already-cleared level for a better grade is always a legitimate, encouraged activity (feeding §7.2's currency loop), not a wasted repeat — the UI (§8) should make "replay for a better grade" as visible an option as "advance to the next level."

---

## 8. UI, HUD & Player Feedback

The HUD exists to make every decision in §3's core loop legible without the player having to open a menu mid-wave. Concretely, it needs:

- **Build menu:** every currently *unlocked* tower (§7.2 — a level always starts with at least the Neutrophil), grouped by functional role (§5.1's second taxonomy) for scannability, each showing cost and a one-line role reminder; disabled/grayed when unaffordable rather than hidden, so the player always sees their full *current* roster and what they're saving toward. A locked tower is a between-run concern (the Strengthen Immunity tree, §7.2), not something the build menu surfaces mid-level.
- **Wave preview panel:** always visible, always at least one wave ahead — family composition (by the same color coding as the horde itself, §6.2/Pillar 2), approximate scale, and an explicit icon when an elite or boss is inbound. This is the single most important piece of UI for making §3.2 step 4's reactive decisions informed rather than reactive-blind.
- **Tower info panel:** appears on selecting a placed tower — its current (permanently tree-boosted) stats, sell (with refund shown up front), and a targeting-mode toggle for Precision-role towers (§5.4). There is no upgrade control here — a tower's power is entirely a function of the Strengthen Immunity tree (§7.2), never a mid-run purchase.
- **Resource readout:** current ATP, income rate, and (during a run) small always-visible Memory Cells / Antibodies counters (§7.2) so the meta-progression loop is felt accumulating in real time, not just revealed at the results screen.
- **Objective integrity meter:** always visible, always legible at a glance as "how close to failing," with an explicit warning state (color shift, not just a number) below a threshold.
- **Active-ability bar:** whichever of the four abilities from §5.6 the player has unlocked so far (zero, early on), each showing cooldown state clearly — this is a small, fixed, always-visible bar, never buried in a menu, since these are meant to be used at exactly the moment the player is under the most pressure.
- **Per-lane readability** (multi-lane levels specifically): per §9.2, the primary channel is the lane's own color/thickness in the world, not a separate abstracted icon — but a small supplementary per-lane indicator (dominant family + rough severity) in a level overview strip is a legitimate addition once in-world readability is solid, especially useful in wide organ-chamber levels where not every lane is on screen at once.
- **Pause/speed controls** and a **results screen** (grade, currency earned, and a direct "replay" or "next level" choice, per §7.4) round out the loop.

---

## 9. Visual & Art Direction

Visual design is not a coat of paint applied after the systems work — every rule below exists because a specific piece of gameplay needs to be *readable*, and the art is the only channel that can carry it at 10,000 agents. If a visual choice doesn't make a mechanic easier to read or react to, it's decoration and should lose to one that does.

### 9.1 Camera & layers

- **Camera:** topdown, tilted ~15–25°, fixed (no free rotation) for a controlled fake-3D read. Soft directional drop-shadows + slight vertical sprite offset sell height without real 3D geometry.
- **Layers (back to front):** tissue substrate (low-contrast, subtle heartbeat pulse) → lane surface (main action plane) → ambient particulate (drifting cytokines/dust, parallax-scrolls for depth, purely decorative, never occludes gameplay-critical info).

### 9.2 Lane identity is a first-class visual system

Per §4.1, every lane in a multi-lane level is a distinct vessel type, and that difference has to be legible from a glance at the whole screen, before the player reads any density or counts any agents:

- Each vessel-type archetype (artery, vein, lymph channel, nerve-adjacent duct, mucosal fold, etc.) gets a **fixed tissue-substrate hue** — warm red-orange for arterial, pale gold for lymphatic, cool violet for nerve-adjacent, and so on — consistent across the whole campaign so a returning player recognizes "that's an artery lane" instantly in a brand-new level.
- The hue lives in the **tissue substrate only** (per §9.1's layering), never in the pathogens themselves — family color-coding (§6.2) must stay legible regardless of which lane a pathogen is currently in, so lane identity and pathogen identity never compete for the same color channel.
- Lane-specific ambient behavior reinforces the identity passively: arterial lanes pulse faster (heartbeat-linked substrate animation), lymphatic lanes drift slower, matching each vessel type's real-world tempo without requiring the player to read anything — it's felt before it's understood.
- **This is also the primary answer to per-lane threat readability**: rather than leaning on an abstracted HUD icon per lane, the lane *is* the readout — a lane under heavy horde pressure visibly thickens, brightens, and pushes warmer/more saturated relative to its resting hue, so "which lane is in trouble" is answered by the same glance that tells you which lane is which. The supplementary HUD strip (§8) exists for wide levels, but the mass itself is always the primary truth.

### 9.3 Palette

Host tissue = warm, low-saturation pinks/creams as the shared baseline "floor," modulated per-lane by §9.2's hue system — the baseline must never compete with foreground regardless of which lane hue is layered over it. Friendly units (towers) = cool blue/white/violet, deliberately the opposite temperature family from every lane hue so towers always read as "not part of the flow." Pathogens = family-coded per §6.2, and that coding is the one constant that never shifts with lane or region.

### 9.4 The fluid feel is a rendering problem as much as a simulation one

§4.2 defines the horde's *behavior* as fluid — pile-up, splash, rejoin, bow waves. None of that lands if the rendering doesn't sell it:

- **Mass, not sprites, at range.** The density-LOD system (chaff resolves to a shader-driven "mass" representation under crowding, individual instances only where density is low) is exactly the right substrate for this — a pile-up or bow wave should read as the *blob* visibly swelling and deforming, the same way a real fluid surface would, not as "more dots stacked in the same spot." The blob shader is where the fluid feel is actually sold, and it should be treated as gameplay-critical rendering, not a performance fallback that happens to look okay.
- **Leading-edge silhouette is the whole story.** A splash-around only reads if the *outline* of the mass is crisp and legible against the tissue substrate at every density level — the moment the leading edge gets muddy, the fluid illusion collapses into "colored fog." Prioritize edge contrast over interior detail everywhere the horde touches an obstacle.
- **Obstacles get their own visible disturbance response**, not just the horde: a Fibrin Clot, a NET edge, or a switchback's outer wall should show a subtle compression/ripple cue right where the horde is pressing against it — this is what makes the pile-up feel like *pressure against something*, rather than the horde just stopping in place.

### 9.5 Combat visibility — the fluid mass must visibly react to being hurt

- **Field VFX:** tower AoEs render as literal fluid/chemical fields (toxin clouds, histamine blooms, antibody tides) that visibly reshape the pathogen river — cause and effect must be visible from a zoomed-out view, this is the core spectacle promise, and per §4.2 the *specific* effect to sell is a visible wake: the mass thins and deforms as it passes through the field, exactly like the splash-around behavior but caused by damage rather than geometry. A damage field and a wall should look like different *causes* of the same kind of visible disturbance, not unrelated effects.
- **Death/impact VFX tiers** (fidelity-budgeted on purpose):
  1. Chaff: dissolve/erosion shader on the color-mass edge where it passes through a damage field — no per-unit death animation. This is the mass "thinning" described above, continuously, not a discrete event.
  2. Elite: individual pop/burst VFX, plus a clearly telegraphed wind-up beforehand (silhouette/color change or a readable warning shape) so an elite's attack is anticipated, not just suffered — telegraphing an attack that the player can't see coming defeats the point of telegraphing it.
  3. Boss: full set-piece animation, plus the persistent lane-obstruction visual noted in §6.4.
- **Tower activity must be visible even when nothing is dying.** A tower with no target should still read as "on" (idle animation, a faint standing field if it has one) versus "off" (disabled/unpowered) — the player should never have to guess whether a placed tower is doing anything.
- **Active abilities (§5.6) get the biggest, most screen-filling VFX in the game**, on purpose — they're rare, player-triggered, and meant to feel like a climactic release of pressure, so their visual budget should exceed even a boss's per-hit effects.

---

## 10. Audio Direction

Audio was previously undocumented; it exists to reinforce the same readability-first principle as art direction (§9), not as a separate creative track.

- **The horde has a collective voice, not per-agent sound.** A dense chaff mass produces a continuous, density-scaled ambient bed (a wet, organic rush/hiss that swells with agent count and lane pressure) rather than thousands of individual sound sources — mirroring the visual mass-not-sprites principle (§9.4) in audio.
- **Family-coded timbre**, mirroring color-coding (§6.2/Pillar 2): each pathogen family contributes a distinct textural layer to the ambient bed (e.g. virus = a higher, faster ticking texture; bacteria = a lower, thicker rumble) so an attentive player can *hear* a wave's composition shift even without looking, especially useful for off-screen lanes in multi-lane levels.
- **Discrete, spot-mixed cues for discrete events**: tower placement/upgrade/sell, an elite's telegraph-to-attack transition (a readable audio tell paired with the visual one, §9.5), a boss phase change, objective-integrity warning threshold, and each active ability's activation — every one of these is a moment §3/§4/§5 already treats as a distinct, important beat, and sound should mark it as such.
- **Adaptive intensity**: an ambient music layer that responds to overall horde density/pressure across the level (not per-lane) — rising tension during a pile-up or bow-wave moment, releasing after a wave clears — reinforcing the pacing curve from §4.5 without needing manual per-level music cues.
- **Mixing priority order** when the screen is at its busiest (matching the visual fidelity-budget principle in §9.5): telegraphed-attack and integrity-warning cues always cut through; the horde's ambient bed is the first thing ducked to make room.

---

## 11. Onboarding & Tutorialization

- The first level (Skin/Epidermis breach, §4.6) is a **single lane, one tower type available, one enemy family** — every subsequent early level in that region introduces exactly one new concept (a second tower, a second family, a bifurcation, a NET-as-Dam moment) rather than front-loading the full roster.
- Tutorialization is delivered **through forced-but-obvious scenarios**, not text boxes: the first bifurcation level is *designed* so that ignoring one branch visibly costs the player, teaching the fork mechanic (§4.4) by consequence rather than instruction. Text hints, where used at all, are short, dismissible, and tied to the specific moment they're relevant to (e.g. a one-line hint the first time a NET is available, explaining its Dam behavior per §5.5) — never a pre-level wall of rules.
- The wave-preview panel (§8) and lane-color-as-threat-readout (§9.2) are both introduced by the first level being simple enough that the player naturally looks at them to succeed, before later levels depend on the player already knowing to look.
- The meta-progression loop (§7.2) is introduced *after* the first failure or first full clear, never before — its value only makes sense once the player has something to compare a "you're now stronger" moment against.

---

## 12. Technical Architecture (C++)

### 12.1 Engine & core stack
- Custom lightweight engine (not a full commercial engine) — favors full control over the mass-agent renderer/simulation, which is the whole point of the project.
- Suggested base libraries: **SDL2** or **GLFW** for windowing/input, **bgfx** or raw **Vulkan/OpenGL** for rendering, **EnTT** for ECS, **Box2D-lite/custom** broadphase only if needed (likely not — see §12.4, movement is field-based, not physics-based).
- Data-oriented design throughout the hot path (agent update, rendering) — this is a performance-first project, OOP-per-enemy is a non-starter at 10k agents.

### 12.2 Agent tiering (the key architectural decision)
- **Chaff agents (~95% of horde):** no individual game-logic tick. Stored in flat SoA (structure-of-arrays) buffers: position, velocity, family ID, HP-as-density-contribution. Updated in batch on the CPU (SIMD-friendly) or offloaded to a compute shader. Rendered via **GPU instancing** (single draw call per family, per-instance transform + color from a buffer). No individual pathfinding — pure flow-field sampling + local separation (cheap boid-lite jitter, not full boids).
- **Named agents (elites/bosses, ~5% or fewer):** full ECS entities with real components (health, AI state machine, telegraphed attacks, individual collision). Standard game-object treatment.
- Damage to chaff is **aggregate**, not per-unit: damage-field towers define a region + kill-rate; chaff agents inside the region are removed probabilistically/by density-threshold per tick rather than needing individual hit registration. This is both a performance necessity and the source of the "erosion" visual language in §9.5.

### 12.3 Movement: flow fields, not pathfinding — and how this produces §4.2's fluid feel for free

- Each level bakes a **vector (flow) field** over its walkable tissue area (offline or on level-load), giving every point a "direction toward the objective" vector — classic RTS/horde-game technique (see *Supreme Commander*, *They Are Billions*).
- A runtime obstruction that blocks part of a lane (a Fibrin Clot; never a tower — towers are not obstacles) triggers a **field rebake** (incremental, only affected region, not a full-level recompute) so the horde reroutes visibly and immediately.
- Chaff agents just sample the field at their position each tick + a small separation impulse from a coarse spatial hash (uniform grid, cell size ≈ agent radius × k) to avoid total stacking. No A*, no per-agent pathing — this is what makes 10k agents tractable.
- Named agents can layer additional local steering (dodge telegraphed attacks, burrow, etc.) on top of the same field.
- **This is the actual mechanism behind the fluid feel in §4.2, not a separate system layered on top.** Pile-up is what a rebake-invalidated region plus unchanged agent inflow already looks like — agents keep sampling a flow field that no longer has a clear forward path near the obstruction, so they slow and compress there by construction. Splash-around falls out of the same sample once the field's gradient bends around the obstruction toward the nearest still-open route — agents don't need a separate "flow around obstacles" behavior, they need the *baked field itself* to already curve that way, which an SDF-aware bake (distance-to-wall feeding the cost field, not just binary walkable/blocked) gives naturally: cost rises smoothly near a wall rather than jumping at the boundary, so the gradient sweeps agents along the wall face instead of routing them to hug it at zero clearance. Bow-wave pile-up upstream of a NET or a Fibrin Clot is the same cost-gradient compression, concentrated into whatever width the obstruction leaves open. **Practical implication: the fluid feel is a tuning target for the flow-field bake and the separation impulse, not a new subsystem** — the region-table (§4.6) split between "gentle pile-up only" and "full spectacle" tunes are different bake/separation parameters on the same underlying system, not different code paths.

### 12.4 Spatial queries
- Uniform grid spatial hash, rebuilt or incrementally updated per frame, used for: separation impulses, AoE/field-tower queries ("which chaff cells overlap this damage field"), targeting queries for named-agent towers (nearest/strongest in range).
- Avoid per-agent-per-tower collision checks; towers query the grid for cells in range, not agents individually.

### 12.5 Rendering pipeline
- Single instanced draw call per (family × animation-frame-batch) — GPU reads a per-instance buffer (position, rotation, color tint, scale) updated from the CPU-side SoA data each frame (or written directly by a compute shader if movement is GPU-side).
- LOD by density, not distance (camera is fixed-ish, so classic distance LOD doesn't apply): when local agent density in a screen region exceeds a threshold, degrade from per-agent instances to a cheaper "density blob" shader representation seamlessly, and back, so the game never has to draw literally 10,000 unique instanced sprites in a tiny area — it fades to a shader-driven mass representation that looks the same but costs far less.
- Field VFX (toxin clouds, histamine blooms) as screen-space or render-target-based shader effects, not particle-per-agent.

### 12.6 Performance targets & budget (rough, to validate in prototyping)
- 10,000 chaff agents: flow-field sample + separation + instanced render, target < 4ms/frame combined on mid-range hardware.
- ≤ 200 named agents: standard ECS tick, target < 2ms/frame.
- Spatial hash rebuild: amortized/incremental, < 1ms/frame.
- Reserve remainder of frame budget for VFX, UI, audio.

### 12.7 Suggested milestone order for prototyping
1. Flow-field generation + single-family chaff movement at 10k agents, no rendering fidelity (colored quads). **Validate the core performance assumption first.**
2. GPU instanced rendering + density-LOD fallback.
3. One tower (aggregate damage field) + erosion death visual.
4. Named-agent ECS layer + one elite type.
5. Lane/level authoring pipeline (how levels define their tissue mask + spawn/objective points, per lane, per §4).
6. Full tower roster (§5) + enemy roster (§6).
7. UI/economy/wave director (§7, §8).
8. Active abilities (§5.6), art/VFX pass (§9), audio (§10), meta-progression (§7.2/§7.3).

---

## 13. Economy & Progression — quick reference

*(§7 is the authoritative, detailed version — and `PROGRESSION.md` is the authoritative version of §7.2/§7.3 specifically; this section is kept short as a cross-reference for anyone jumping straight to "what's the resource model.")* In-level resource ("ATP") earned passively + per-kill, spent on tower **placement only** — no in-run upgrades (§5.3, §7.1). Two cross-run meta-currencies feed one permanent tree, **Strengthen Immunity**: **Memory Cells** (every run, funds incremental stat nodes) and **Antibodies** (a level's first clear only, funds tower/ability unlocks and branch capstones) (§7.2, `PROGRESSION.md`). Cosmetic pathogen/tissue skins per region are a plausible optional reward layer on top of this, not yet load-bearing for any system above.

---

## 14. Open Questions

- Single-player campaign only, or an endless/horde-survival mode as a secondary pillar (likely a strong fit given the tech, and partially answered by §4.5's per-level endless extension — the open question is whether a *standalone*, level-independent endless mode is also worth building)?
- Co-op (two players, shared vessel network, split lane responsibility)?
- Precise input/targeting scheme for tower placement given the grid-free continuous lane surface — needs a prototype pass.
- Exact aggregate-damage-to-chaff formula (probabilistic removal vs. deterministic density thinning) — needs a feel pass once §12.7 milestone 3 is playable.
- Exact lane count per level/region, and how strict the "forks/merges at most once" rule (§4.1) should be — worth stress-testing an actual 2-fork or hub-with-5-lanes layout before locking it as a hard rule versus a strong default.
- Tuning the fluid-feel parameters (§12.3) to actually produce a convincing pile-up/splash/rejoin at each region's intended intensity (§4.6) is un-prototyped — this is the single highest-risk "does it actually feel right" item in the whole document and should be validated early, on one switchback and one floodplain lane, before content production leans on the feel being correct everywhere.
- How much does a lane's vessel-type identity (§9.2) drive gameplay versus purely reinforcing recognition? Worth deciding explicitly whether it should carry a mild passive modifier (e.g. arterial lanes running faster, changing available reaction time) or stay cosmetic-only.
- **Exact Memory Cell/Antibody earn-rate curves and the full Strengthen Immunity cost table** (§7.2, `PROGRESSION.md`) — the loop and tree structure are fully specified, the numbers are not; needs its own balance pass once the tree is implementable end to end, distinct from the general numeric-balance deferral below.
- **How many boss archetypes does the campaign need**, and do they share a kit family or should each organ chamber's boss be mechanically unique? §6.4 gives a design framework, not a roster.
- **What does the enemy roster actually contain?** (§6.2/§6.3) — the roster has been cut back to two chaff families (Virus, Bacteria) and no elites at all, pending a redesign. Bacteria has an empty behavior slot; the elite and boss tiers are empty frameworks. Everything added back should follow §6.3's rule and name the defensive gap it tests first, and §6.5's rule that each region introduce at least one new family or elite has nothing to draw on until then.
- **Tree balance specifics deferred to `PROGRESSION.md`'s own open questions** — respec cost (if any), whether Antibodies should trickle from endless/overtime play (§4.5) once the whole tree is unlocked, and exact per-line level counts and costs.

---

*This document covers vision, content, and the technical strategy required to hit the 10k-agent target. Numeric balance (exact damage values, costs, wave curves, and the Memory Cell/Antibody earn/spend economy — see `PROGRESSION.md`) is intentionally left for a post-prototype tuning pass — this pass adds the *structure* those numbers need to slot into, not the numbers themselves.*
