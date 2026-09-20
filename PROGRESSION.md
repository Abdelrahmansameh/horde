# IMMUNE — Progression Design: Strengthen Immunity

**Companion to `DESIGN.md`.** This document is the authoritative design for §7.2/§7.3 of `DESIGN.md` and for the unlock-gating referenced in §5.3 and §5.6 — those sections summarize; this one specifies. It is a design document, not an implementation plan: it describes what the system is and why, in enough detail that a technical architect can derive data schemas, save-file shape, and a build plan from it without having to make content decisions along the way. It does not name files, structs, or code.

Where this document references something that already exists in the game (the current tower roster, the four active abilities, the economy's shape), that's stated as context, not as a constraint the architect must preserve byte-for-byte — it's there so nothing below reads as inventing a system from nothing.

---

## 1. What changed, and why

Previously, ATP spent *in a level* bought both new towers and tier-upgrades for towers already placed, with the tier-upgrade path deliberately cheaper (an economic nudge borrowed from *Sir, We Have an Orc Problem*'s "upgrade before expanding" lesson). Every tower started unlocked, starting ATP was set per level, and a separate "loadout" system let players pick a handful of temporary modifiers before each run.

All of that in-run power growth is removed. What replaces it:

- A player starts a new campaign with **one tower (Neutrophil)** and **zero active abilities**.
- Every other tower, every active ability, and every stat improvement on anything is bought permanently, between runs, from one shared skill tree: **Strengthen Immunity**.
- ATP, in a level, buys **placement only**. Nothing about a tower changes once it's on the field except what the player already owns permanently.
- Starting ATP and both income rates are **global constants raised by the tree**, never set per level.

The intent, matching the request this document was written to satisfy: the player starts the campaign's first level expected to lose. Losing still pays out (Memory Cells, below), so the player returns to Strengthen Immunity, buys what they can afford, and tries again — genuinely stronger, not just "more practiced." Clearing a level for the first time is the moment that matters most: it pays out the rarer currency (Antibodies) and opens the next level.

## 2. Thematic frame

You start with only **innate** immunity — fast, blunt, always on, no memory of anything. The Neutrophil is the correct starting tower for this on both counts: it's cheap and simple to use alone, and it's biologically the actual first responder cell recruited to a site of infection.

Strengthen Immunity is your **adaptive** immune system maturing through repeated exposure. Every other cell type, every active response, and every refinement to an existing one is something your body only learns to produce reliably after surviving real fights — which is exactly what a skill tree bought with post-run currency represents. A level you've fully cleared for the first time is framed as **seroconversion**: your body has now produced a working countermeasure against that specific threat, which is where the rarer of the two currencies comes from.

## 3. The two currencies

### 3.1 Memory Cells

- **Earned:** from every run, cleared or failed, scaled by in-run performance (waves cleared, pathogen density killed, elites/bosses defeated). A wipe on the very first wave still earns something — the floor is never zero. Winning adds a bonus on top; it never gates earning to zero. (This reuses the shape of the performance-scored payout that already existed for the single pre-existing currency — same inputs, same "always something" guarantee, just renamed and now one of two.)
- **Spent on:** every leveled stat node in the tree — every tower's damage/rate/range/health/count lines, the hub branch's economy lines, and the potency/cooldown lines beneath each unlocked active ability.
- **Feel:** plentiful and incremental. A player should be able to afford *something* after almost any attempt, even a bad one.

### 3.2 Antibodies

- **Earned:** exactly once per level, on that level's **first** clear. Replaying an already-cleared level, at any grade, earns zero additional Antibodies. (Grade — §7.4 of `DESIGN.md` — still affects Memory Cells and bragging rights; it never affects Antibody count.)
- **Spent on:** the tree's milestone nodes only —
  1. Unlocking one of the five towers the player doesn't start with.
  2. A tower branch's capstone node.
  3. Unlocking one of the four active abilities.
- **Feel:** rare and milestone-shaped. Earning one should read as "I beat something," not "I ground for a while." Because there's roughly one Antibody per level and a bounded number of Antibody-gated nodes (§5), the player is guaranteed to be making a real choice about what to unlock next rather than eventually affording everything at once.

### 3.3 Why two currencies instead of one

A single currency funding everything makes "which tower do I unlock next" and "do I take another point of Neutrophil damage" compete for the same pool, which either trivializes the tower-unlock decision (currency is abundant, so of course you buy the new tower) or starves it (currency is scarce, so incremental stat purchases feel like they're stealing from a unlock you want more). Splitting them removes the competition: stat growth is always available and always feels good to spend on, while "what's my next big capability" is paced by campaign progress specifically, which is the axis the campaign is actually built around.

## 4. Tree shape

Strengthen Immunity is a single connected graph, not a flat shop list, and it's meant to be *drawn* as one — a branching, vascular/dendritic diagram radiating from a central hub, which costs nothing extra conceptually and pays for itself visually (it's exactly the kind of diagram the game's existing art direction already leans on for lane and vessel shapes).

```
                              [ HUB ]
                 economy lines + 4 ability unlocks
               /       |        |        |        \
        Neutrophil  Macrophage Interferon Goblet  Fibroblast
        (owned from   (Antibody  (Antibody  Cell    (Antibody
         start)        to open)  to open)  (Antibody  to open)
           |               |         |     to open)     |
       [stat lines]   [stat lines][stat lines][stat lines][stat lines]
           |               |         |         |            |
       [capstone]     [capstone][capstone] [capstone]   [capstone]
       (Antibody-gated, one per branch)
```

### 4.1 Node types

- **Root (unlock) node.** One per tower branch. Neutrophil's is already owned at campaign start; the other five each cost a small number of Antibodies and have **no prerequisite on any other root** — every locked tower is purchasable the moment the player has an Antibody to spend, in whatever order they want. This is the direct answer to "you can choose the order in which you unlock the towers."
- **Stat (leveled) node.** Multiple per branch, each representing one upgradeable dimension of that tower, bought in discrete levels (roughly 3-5 per line) at Memory Cell cost that increases per level. Different lines within the same branch have no prerequisites on each other — a player can dump everything into Damage and ignore Range, or spread evenly.
- **Capstone node.** One per branch, gated behind (a) the branch's root being owned and (b) a minimum number of points already spent somewhere in that branch (a simple threshold, not a specific chain of prior nodes — e.g. "6 points spent in this branch," exact number is a balance call). Costs both Antibodies and Memory Cells. This is the build-defining, unique-effect purchase at the end of investing in one tower, distinct in kind from the stat lines feeding it.
- **Hub nodes.** Economy lines (Memory-Cell-only, no prerequisites, always available) and ability-unlock nodes (Antibody-gated roots, exactly like a tower root, each with its own Memory-Cell-funded stat lines beneath it).

### 4.2 What's deliberately *not* in the tree (v1)

- **Cross-branch synergy nodes** — small bridge nodes requiring points in two adjacent branches (e.g., something between Interferon and Goblet Cell reading "slowed targets count as marked"). Good future texture once the base tree exists and its balance is understood; not required to make the tree function, and adding it later is additive, not a rework.
- **Respec / refund.** Recommended for inclusion (a tree this size benefits from letting players correct an early mistake, and the game already has a refund-fraction precedent for tower sell/placement) but the cost/availability of a respec is a balance call, not a structural one — see §8.

## 5. Tower branches

Each branch's stat lines are drawn directly from what's already tunable per tower in the current roster (currently expressed as per-tier values; in this design those tiers collapse into one baseline plus tree-purchased levels on top — see §7). Every branch follows the same shape: an unlock root (already owned for Neutrophil), a set of independent stat lines, one capstone.

### 5.1 Neutrophil (Shooter) — owned from start

| Line | What it governs |
|---|---|
| Round Damage | per-shot damage |
| Volley Cadence | how often the tower releases a new swarmer squad |
| Trigger Rate | how fast an active swarmer fires once deployed |
| Aggro Range | how far a swarmer will search for a target |
| Squad Size | swarmers released per volley |
| Accuracy | shot spread reduction |
| Swarmer Vitality | swarmer health/lifetime in the field |
| Tower Health | the tower's own max HP |

**Capstone — Incendiary Rounds:** impacts leave a brief burning patch, adding an Erosion-role effect to a tower that's otherwise pure Dam+Erosion-by-volume — see `DESIGN.md` §5.1 for the fluid-role taxonomy this is playing into.

### 5.2 Cytotoxic T (Latch) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Drain DPS | damage per second while latched |
| Attach Speed | how quickly a swarmer locks onto its target |
| Deploy Cadence | how often the tower releases new latchers |
| Search Radius | how far a latcher will hunt |
| Squad Size | latchers released per volley |
| Swarmer Speed/Lifetime | how long/fast a latcher can hunt before expiring |
| Tower Health | the tower's own max HP |

**Capstone — Apoptosis Trigger:** a kill releases a small damage pulse to nearby chaff, and the tower gains a flat bonus vs. elites/bosses — restoring the "precision, bonus vs. named threats" fiction this cell originally had.

### 5.3 Macrophage (Arbor Grabber) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Arm Count | how many independent pseudopod trees grow at once |
| Extend/Latch/Pull/Recover Speed | how fast one arm's grab cycle completes |
| Deploy Cadence | how often the tower releases new bodies |
| Search Radius | how far a body will reach for a target |
| Body Count | how many macrophage bodies are active at once |
| Body/Tower Health | HP of both the released bodies and the tower itself |
| Wall Spacing/Body Block | how tightly a volley's LANE WALL packs and how much it shoves the horde back rather than yielding |

**Capstone — Phagocytic Sustain:** a kill heals the tower — the "eats what it kills" fiction made mechanical.

### 5.4 Interferon (Slow bomber) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Slow Potency | how much a slow zone reduces enemy speed |
| Slow Duration | how long the slow lingers after leaving the zone |
| Zone Radius | the slow field's size |
| Zone Duration | how long the field itself persists |
| Deploy Cadence | how often the tower drops a new zone |
| Tower Health | the tower's own max HP |

**Capstone — Cytokine Storm:** targets currently slowed take bonus damage from *every* tower on the field, not just this one — a systemic synergy payoff, the first of two "amplify a status effect globally" capstones (see §5.5 for the other).

### 5.5 Goblet Cell (Mucus bomber) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Splash DPS | damage per second in the mucus splash |
| Splash Radius | the splash's size |
| Droplet Count | droplets released per shot |
| Mark Duration | how long a hit target stays marked |
| Deploy Cadence | how often the tower fires |
| Tower Health | the tower's own max HP |

**Capstone — Anaphylactic Shock:** a marked target's death spreads the mark to nearby chaff — revives the original marking-combo fiction (`DESIGN.md` §5.5) as a spreading effect rather than a flat damage amplifier, so it reads distinctly from Interferon's capstone rather than duplicating it.

### 5.6 Fibroblast (Builder) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Scar Health | HP of a placed scar |
| Reinforce Rate | how fast a builder repairs a standing scar |
| Max Scars | how many scars this tower can have standing at once |
| Scar Size | length/width of a placed scar |
| Build Radius | how far from the tower a scar can be placed |
| Build Cadence | how often the tower sends a new builder |
| Tower Health | the tower's own max HP |

**Capstone — Inflammatory Scarring:** scars deal contact damage / apply a slow to anything pressed against them, turning a pure Dam tower into a Dam+Erosion hybrid late-game.

## 6. Hub branch

Cheap, broad, prerequisite-free — the first thing any player buys, struggling or not.

**Economy lines (Memory Cells):**

- **Bone Marrow Reserve** — starting ATP
- **Rapid Metabolism** — passive ATP/sec
- **Efficient Clearance** — ATP earned per density killed
- **Field Requisition** — global tower build-cost reduction
- **Systemic Potency** — small global damage % across all towers (deliberately capped low so it complements per-tower investment rather than replacing it)
- **Cellular Resilience** — global tower max-HP %
- **Rapid Deployment** — refund fraction increase on sell
- **Elite Response** — global bonus damage vs. elites/bosses
- **Homeostasis** — objective integrity max, or leak-damage reduction

**Active ability unlocks (Antibody-gated roots, Memory-Cell-funded lines beneath each):**

All four of the game's existing player-triggered abilities move here, unlocked permanently instead of available from the start:

| Ability | Unlock (Antibody) | Lines (Memory Cells) |
|---|---|---|
| **Complement Cascade Burst** | unlock node | Cooldown Reduction · Blast Potency · Chain Link count |
| **Histamine Flare** | unlock node | Cooldown Reduction · Radius · Potency/Duration |
| **Fever Response** | unlock node | Cooldown Reduction · Buff Magnitude · Buff Duration |
| **Fibrin Clot** | unlock node | Cooldown Reduction · Barrier Duration · Barrier Width |

Once unlocked, an ability is simply always present on the in-run ability bar — there is no per-run selection of *which* unlocked abilities to bring (that would be exactly the loadout-style choice this design deliberately removed, §3 of `DESIGN.md` §7.3).

## 7. Consequence for in-run tower data

This is a design constraint stated for the architect's benefit, not a schema: a tower's in-run stats must resolve to **one baseline value per stat, permanently modified by whatever the player has purchased for that tower type**, evaluated once at run start (or, if levels are re-enterable mid-session without a restart, at whatever point makes tree purchases retroactive to already-placed towers of that type — a call for the implementation plan, not this document). What currently exists as three discrete tiers per tower should collapse to a single baseline per tower; the tree's leveled stat nodes are what used to be tier deltas, just permanent and cross-run instead of paid for mid-level.

## 8. Open questions

These are genuinely undecided and intentionally deferred to a balance/implementation pass, consistent with `DESIGN.md`'s own policy of specifying structure before numbers:

- **Respec.** Should Strengthen Immunity purchases ever be refundable? If so, at what cost (flat Memory Cell fee, partial-refund-and-rebuy, free-but-limited-uses)? Recommended default: allow it, at a Memory Cell cost, since a tree this size will produce early mistakes a new player can't be expected to foresee — but this is a recommendation, not a lock.
- **Antibody trickle post-campaign.** Once every tower, capstone, and ability is unlocked, all Antibody-gated content is exhausted. Should the endless/overtime extension mode (`DESIGN.md` §4.5) grant a small ongoing Antibody trickle so nothing about the tree is permanently unspendable for a completionist player, or is "the tree eventually finishes" an acceptable end state?
- **Per-line level counts and costs.** This document specifies which lines exist, not how many levels each has or what they cost — that's the numeric balance pass `DESIGN.md` defers everywhere else.
- **Capstone threshold.** The exact "points spent in this branch" number gating a capstone purchase.
- **Antibody count per tower/ability unlock and per capstone.** Whether every Antibody-gated node costs the same flat amount, or milestone nodes scale in cost — matters for how quickly a campaign-length player can realistically unlock everything.
- **Existing tooling fallout.** The project's balance-testing harness currently includes at least one automated play-strategy specifically built around the old in-run tier-upgrade path ("deepen existing towers before adding new ones," ranking towers by solo performance). That strategy's premise no longer applies and will need rethinking once tiers move to the tree — flagged here as a downstream implication of this design, not something this document resolves.

---

*This document specifies the progression system's shape and content. It intentionally omits save-file format, data schema, and code structure — that translation is the implementation plan's job, built from this and from `DESIGN.md`.*
