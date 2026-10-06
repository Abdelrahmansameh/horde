# IMMUNE — Progression Design: Strengthen Immunity

**Current deployment update:** Roster roots now unlock directly placed cells.
In-run ATP buys individual cells, which persist without an age limit. The
implemented progression tree uses per-cell health, damage, movement, and
placement cost bonuses in place of tower cadence, volley size, tower health,
and lifetime bonuses. Older tower wording below is historical context.

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
  1. Unlocking one of the four towers the player doesn't start with.
  2. A tower branch's capstone node.
  3. Unlocking one of the four active abilities.
- **Feel:** rare and milestone-shaped. Earning one should read as "I beat something," not "I ground for a while." Because there's roughly one Antibody per level and a bounded number of Antibody-gated nodes (§5), the player is guaranteed to be making a real choice about what to unlock next rather than eventually affording everything at once.

### 3.3 Why two currencies instead of one

A single currency funding everything makes "which tower do I unlock next" and "do I take another point of Neutrophil damage" compete for the same pool, which either trivializes the tower-unlock decision (currency is abundant, so of course you buy the new tower) or starves it (currency is scarce, so incremental stat purchases feel like they're stealing from a unlock you want more). Splitting them removes the competition: stat growth is always available and always feels good to spend on, while "what's my next big capability" is paced by campaign progress specifically, which is the axis the campaign is actually built around.

## 4. Tree shape

Strengthen Immunity is one tree, and its nodes, joined by vessels that branch outward from the heart the way arteries do, draw the body it strengthens: no outline, the pattern of nodes alone makes a human figure. The Neutrophil (innate immunity, owned from the start) is the heart, ringed by its own lines like ribs; the hub lines run up the sternum and the neck and down the middle of the belly; two abilities make the ring of the head and two run down the sides of the waist; each of the other four towers is a limb, from its unlock at the shoulder or hip down to its capstone at the hand or foot.

```
                      Radius               Healing Strength
                   Cooldown                     Cooldown                    head: a ring,
              [Histamine Flare]          [Fever Response]                   an ability up
                              \          /                                  each side
                             [ Homeostasis ]
                                    |
                            [ Elite Response ]                              neck
                                    |
   [Cytotoxic T] -------- [ Bone Marrow Reserve ] -------- [Goblet Cell]    shoulders
   left arm, capstone                  |                   right arm, capstone
   in the hand                [ NEUTROPHIL ] the root      in the hand
                                       |
       ribs: Neutrophil ------ [ Round Damage ] ------ ribs: Neutrophil     chest
       lines, capstone                 |               lines
   [Complement Cascade] -- [ Rapid Metabolism ] -- [Fibrin Clot]            belly: an
    Cooldown, Chain Links              |            Cooldown, Duration       ability down
                                       |                                     each side
                          [ Efficient Clearance ] -- Field Requisition,
                                       |             Systemic Potency
                          [ Membrane Resilience ]                           pelvis
                               /               \
                     [Macrophage]             [Fibroblast]                  hips
                     left leg, capstone       right leg, capstone
                     at the foot              at the foot
```

The rules:

- **One parent each.** Every node hangs off exactly one parent; the Neutrophil, the root, has none. No node has more than **three** children.
- **A node opens when its parent is owned** (at any level). That is the only prerequisite besides a capstone's branch-point threshold (§4.1).
- **Milestones stay the player's choice.** The path from the heart to every tower and ability unlock runs through Memory Cell nodes only, never through another Antibody node, so the order towers and abilities are unlocked in is still the player's (the direct answer to "you can choose the order in which you unlock the towers"). Where a milestone sits sets how soon it can be reached: the arms are one cheap node from the heart, the belly abilities two, the head's three, the legs four.
- **Capstones end a limb.** Each is a leaf at the end of its branch's chain (the hands, the feet, the top of the ribs).
- **The tree uncovers as it grows.** The screen shows the owned nodes and the nodes they feed -- what can be bought next -- and hides everything past them. A new campaign sees the heart and its two children, zoomed in; the figure takes shape as it grows. The view pans and zooms out to the whole figure.

### 4.1 Node types

- **Root (unlock) node.** One per tower branch. Neutrophil's is already owned at campaign start and is the root of the whole tree; the other four each cost a small number of Antibodies and sit at a shoulder or a hip, reached through hub (Memory Cell) nodes only.
- **Stat (leveled) node.** Several per branch, each one upgradeable dimension of that tower, bought in discrete levels (roughly 3-5 per line) at a Memory Cell cost that rises per level. A branch's lines form a short chain down its limb, with a bud or two off it; one level in a line opens the lines after it.
- **Capstone node.** One per branch, at the end of its limb, gated behind (a) its parent and (b) a minimum number of points already spent anywhere in that branch (a simple threshold, e.g. "6 points spent in this branch"; exact number is a balance call). Costs both Antibodies and Memory Cells. This is the build-defining, unique-effect purchase at the end of investing in one tower, distinct in kind from the stat lines feeding it.
- **Hub nodes.** Economy lines (Memory Cells) along the spine, and ability-unlock nodes (Antibody-gated roots, exactly like a tower root, each with its own Memory-Cell-funded stat lines beneath it).

### 4.2 What's deliberately *not* in the tree (v1)

- **Cross-branch synergy nodes** — small bridge nodes requiring points in two adjacent branches (e.g., something between the Goblet Cell and the Fibroblast reading "slowed targets count as marked"). Good future texture once the base tree exists and its balance is understood; not required to make the tree function, and adding it later is additive, not a rework.
- **Respec / refund.** Recommended for inclusion (a tree this size benefits from letting players correct an early mistake, and the game already has a refund-fraction precedent for tower sell/placement) but the cost/availability of a respec is a balance call, not a structural one — see §8.

## 5. Tower branches

Each branch's stat lines are drawn directly from what's tunable per tower in the current roster. Every branch follows the same shape: an unlock root (already owned for Neutrophil), a chain of stat lines down its limb, one capstone at the end of it.

The tree was trimmed (2026-10) by the lines that duplicated another: every tower's own placement-cost line (Field Requisition, §6, does that for all of them), a second health line where a branch had two, the Cytotoxic T's second drain line, the Fibroblast's second scar-health line, the Goblet Cell's droplet count and the Macrophage's Body Mass. The tables below list what is left.

### 5.1 Neutrophil (Shooter) — owned from start

| Line | What it governs |
|---|---|
| Round Damage | per-shot damage |
| Trigger Rate | how fast an active swarmer fires once deployed |
| Aggro Range | how far a swarmer will search for a target |
| Squad Size | swarmers released per volley |
| Accuracy | shot spread reduction |
| Swarmer Vitality | swarmer health/lifetime in the field |

**Capstone — Incendiary Rounds:** impacts leave a brief burning patch, adding an Erosion-role effect to a tower that's otherwise pure Dam+Erosion-by-volume — see `DESIGN.md` §5.1 for the fluid-role taxonomy this is playing into.

### 5.2 Cytotoxic T (Latch) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Drain DPS | damage per second while latched |
| Attach Speed | how quickly a swarmer locks onto its target |
| Search Radius | how far a latcher will hunt |
| Swarmer Speed/Lifetime | how long/fast a latcher can hunt before expiring |
| Tower Health | the tower's own max HP |

**Capstone — Apoptosis Trigger:** a kill releases a small damage pulse to nearby chaff, and the tower gains a flat bonus vs. elites/bosses — restoring the "precision, bonus vs. named threats" fiction this cell originally had.

### 5.3 Macrophage (Arbor Grabber) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Arm Count | how many independent pseudopod trees grow at once |
| Extend/Latch/Pull/Recover Speed | how fast one arm's grab cycle completes |
| Captive Capacity | how many pathogens one body can hold and be killing simultaneously |
| Search Radius | how far a body will reach for a target |
| Body/Tower Health | HP of both the released bodies and the tower itself |
| Wall Spacing/Body Block | how tightly a volley's LANE WALL packs and how much it shoves the horde back rather than yielding |

**Capstone — Phagocytic Sustain:** a kill heals the tower — the "eats what it kills" fiction made mechanical.

### 5.4 Goblet Cell (Mucus bomber) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Slow Strength | how sharply mucus reduces movement speed |
| Splash Radius | the splash's size |
| Slow Duration | how long a soaked target stays slowed after leaving mucus |
| Weakening Mucus | a slowed target takes more damage from every source for as long as the slow lasts -- the Goblet Cell's answer to "a slow on its own kills nothing" |
| Tower Health | the tower's own max HP |

**Capstone — Anaphylactic Shock:** a slowed target's death spreads the slow to nearby chaff, extending the Goblet Cell's lane-control role.

### 5.5 Fibroblast (Builder) — Antibody-gated unlock

| Line | What it governs |
|---|---|
| Scar Health | HP of a placed scar |
| Reinforce Rate | how fast a builder repairs a standing scar |
| Scar Size | length/width of a placed scar |
| Build Radius | how far from the tower a scar can be placed |
| Inflammation | a standing scar inflames the tissue around it: allied swarmers there hit harder and towers there reload faster, so walls become the anchor a defence is built around |
| Tower Health | the tower's own max HP |

**Capstone — Inflammatory Scarring:** scars deal contact damage / apply a slow to anything pressed against them, turning a pure Dam tower into a Dam+Erosion hybrid late-game.

## 6. Hub branch

Cheap and broad: the spine of the body (§4), and the first thing any player buys, struggling or not.

**Economy lines (Memory Cells):**

- **Bone Marrow Reserve** — starting ATP
- **Rapid Metabolism** — passive ATP/sec
- **Efficient Clearance** — ATP earned per density killed
- **Field Requisition** — global tower build-cost reduction
- **Systemic Potency** — small global damage % across all towers (deliberately capped low so it complements per-tower investment rather than replacing it)
- **Elite Response** — global bonus damage vs. elites/bosses
- **Homeostasis** — objective integrity max, or leak-damage reduction
- **Membrane Resilience** — reduces damage taken by towers and swarmers from hostile pathogen attacks (Virus latch, Bacteria toxin shots — `DESIGN.md` §5.7), globally across the whole roster. This line didn't exist when this document was first written; towers and swarmers used to be untouchable, and now the horde can kill them back, so surviving that is now a real thing to invest in rather than a placement afterthought.

**Active ability unlocks (Antibody-gated roots, Memory-Cell-funded lines beneath each):**

All four of the game's existing player-triggered abilities move here, unlocked permanently instead of available from the start:

| Ability | Unlock (Antibody) | Lines (Memory Cells), the second growing from the first |
|---|---|---|
| **Complement Cascade Burst** | unlock node | Cooldown Reduction · Chain Link count |
| **Histamine Flare** | unlock node | Cooldown Reduction · Radius |
| **Fever Response** | unlock node | Cooldown Reduction · Healing Strength |
| **Fibrin Clot** | unlock node | Cooldown Reduction · Barrier Duration |

Once unlocked, an ability is simply always present on the in-run ability bar — there is no per-run selection of *which* unlocked abilities to bring (that would be exactly the loadout-style choice this design deliberately removed, §3 of `DESIGN.md` §7.3).

## 7. Consequence for in-run tower data

Each tower type has **one baseline value per stat**, permanently modified by purchases for that type. The tree's leveled stat nodes provide cross-run growth instead of in-run tier upgrades. At run start, the game copies the baseline config and applies the player's purchases to that copy.

## 8. Open questions

These are genuinely undecided and intentionally deferred to a balance/implementation pass, consistent with `DESIGN.md`'s own policy of specifying structure before numbers:

- **Respec.** Should Strengthen Immunity purchases ever be refundable? If so, at what cost (flat Memory Cell fee, partial-refund-and-rebuy, free-but-limited-uses)? Recommended default: allow it, at a Memory Cell cost, since a tree this size will produce early mistakes a new player can't be expected to foresee — but this is a recommendation, not a lock.
- **Antibody trickle post-campaign.** Once every tower, capstone, and ability is unlocked, all Antibody-gated content is exhausted. Should the endless/overtime extension mode (`DESIGN.md` §4.5) grant a small ongoing Antibody trickle so nothing about the tree is permanently unspendable for a completionist player, or is "the tree eventually finishes" an acceptable end state?
- **Per-line level counts and costs.** This document specifies which lines exist, not how many levels each has or what they cost — that's the numeric balance pass `DESIGN.md` defers everywhere else.
- **Capstone threshold.** The exact "points spent in this branch" number gating a capstone purchase.
- **Antibody count per tower/ability unlock and per capstone.** Whether every Antibody-gated node costs the same flat amount, or milestone nodes scale in cost — matters for how quickly a campaign-length player can realistically unlock everything.
- **Whether "Membrane Resilience" (§6) is enough, or hostile-pressure survivability needs per-tower lines too.** The hub's flat global line is the minimum viable answer to towers/swarmers now being killable (`DESIGN.md` §5.7); it's an open question whether a tower that leans into standing its ground (the Macrophage, whose whole kit is built around not yielding) should get its own branch-specific resilience line instead of or in addition to the global one.
- **Whether a "marked" weaken debuff should get a real source again.** `DESIGN.md` §5.5 notes the mechanism exists in code but nothing currently sets it, now that the Goblet Cell deals no damage. *Partly answered:* the Goblet Cell's Weakening Mucus line (§5.4) makes slowed targets take more damage, tied to the slow rather than to the old permanent mark. Whether the permanent mark itself should come back is still open.
- **Whether a burrowed Parasite should ever have a hard counter.** `DESIGN.md` §5.7/§14 raises this; if the answer is yes, the likely home is a capstone or ability unlock in this tree (an "anti-stealth" node echoing the retired NK Cell's niche) rather than a change to the base roster — undecided pending that call.

---

*This document specifies the progression system's shape and content. It intentionally omits save-file format, data schema, and code structure — that translation is the implementation plan's job, built from this and from `DESIGN.md`.*
