# IMMUNE — the balance harness

Balancing this game used to mean playing a level and forming an impression.
This is the alternative: a bot plays levels headlessly at full CPU speed, and
every run emits an exhaustive JSON record of what each tower earned, what each
pathogen family cost, and where the pressure actually was.

The whole thing exists to answer comparative questions. A single run is a
sample; the harness is built for the differences *between* runs.

---

## The two commands

Play one level and read the report:

```bash
immune --autoplay --level assets/levels/skin_1_breach.json --report run.json
```

Sweep the content set and get a digest:

```bash
python tools/balance_sweep.py --seeds 3 --profiles all
```

The sweep writes `tools/balance/summary.md` (the digest), `summary.csv` (one
row per run), and `runs/*.json` (every raw report). All of it is gitignored:
a report is only meaningful against one `config_hash`, so it is a local
artifact, regenerated after every tuning change.

---

## Watching the bot

A bot whose play cannot be inspected cannot be trusted to produce balance
numbers, so the same bot drives the windowed game:

```bash
immune --level assets/levels/skin_1_breach.json --exec "autoplay on; time 8"
```

`autoplay on|off [profile]` and `time <scale>` are gym console commands
(docs/GYM.md), so they also work from the panel mid-run. This is the same
`AutoPlayer` object the headless run uses, driven from the same place a
player's clicks are applied — what you watch is what the harness measures.

---

## Strategies

The bot never uses active abilities and never sells. It buys, on a half-second
cadence, and which thing it buys is the strategy:

| `--profile` | Behaviour | What it is for |
|---|---|---|
| `greedy-cheapest` *(default)* | Always the cheapest affordable item in the plan | The baseline "average player" |
| `spread-coverage` | Fill every planned site before upgrading anything | Tests breadth |
| `save-for-tier3` | Deepen existing towers before adding new ones | Exposes an underpriced tier 3 |
| `single-type:<tower>` | Build one tower type and nothing else | Ranks the six towers with everything else held constant |

The **spread between profiles on one level** is the most informative single
output:

- one profile wins where five lose → a dominant strategy the design did not intend;
- every profile wins comfortably → the level is not asking anything;
- every profile loses → the level or the pricing is out of reach.

The six `single-type` runs are the cleanest tower-vs-tower comparison
available, because the level, the seed, and the site geometry are identical
across them.

---

## Where the bot builds

Entirely derived from the level — there is no per-level plan file to keep in
step with level edits, by design (a stale plan produces a reading that
describes the plan rather than the level).

Each vessel spline is sampled, and each sample scored on four things the level
already knows about itself:

| Term | Weight | Source |
|---|---|---|
| Chokepoint — how narrow the lumen is | 1.0 | `VesselPoint::width` |
| Lateness — how close to the objective along the *actual* path | 0.6 | `FlowField::sample_cost` |
| Lane coverage — how many lanes are in range | 0.5 / extra lane | `LaneOwnershipMap` |
| Author's hint | 0.8 × priority (doubled if `concentrated`) | `PlacementZoneTag` |

Sites are then accepted greedily with a spacing rule (0.7 × tier-1 range) so
the plan spreads rather than stacking towers on the single best cell, and each
site draws the tower type whose `family_mask` best covers the level's own wave
table, with diminishing returns per type so one strong tower does not take
every site and leave five untested.

Every site is checked through `TowerSystem::validate()` — the same call the
build cursor makes. The bot has no path into the sim a player lacks.

The trade this makes: a decent generalist rather than an expert on any one map.
That is the right trade for a comparative measurement, and it is why the
harness reports across seeds and strategies rather than trusting one run.

---

## Reading a report

Per-run JSON, schema 1. The header pins provenance — `level`, `seed`,
`profile`, `config_hash`, `config_dir` — because a number without those four is
a number without a claim attached.

### `towers_by_type` — the ranking table

The one to read first.

- **`atp_per_density`** — ATP invested per unit of chaff density removed. Lower
  is better value, and it is the only figure that compares a Neutrophil to a
  Macrophage without first asking how many of each got built. If two towers
  differ by more than about 2×, one of them is mispriced.
- **`uptime`** — the fraction of its life the tower removed anything at all. A
  strong tower with low uptime is not strong, it is badly placed or badly
  ranged; a *price* set from its damage alone will be wrong.
- **`instances: 0`** — no strategy on any level ever thought this tower was
  worth buying. That is a pricing or power finding on its own.

### `towers` — per instance

Every placement, retained after a sell. Position, lane, build tick, tier
timeline, ATP invested, damage split by family, kills, active vs. alive ticks.
Use it when the type-level number looks wrong and you need to know whether it
was one bad site dragging the average.

### `families` — what the level threw and what became of it

`spawned` / `killed` / `leaked` / `despawned_out_of_bounds` reconcile exactly
against each other (a test asserts it). `leak_rate` next to `density_removed`
distinguishes a family surviving because it out-tanks the towers from one
surviving because nothing ever reached it.

### `waves` — where the difficulty is

`integrity_cost` is the wave's real price. `leak_rate`, `peak_chaff`, and
`peak_density` say whether it was volume or durability. `atp_earned` vs.
`atp_spent` says whether the player could respond to it at all.

The sweep's digest flags any wave costing more than one standard deviation
above its own level's mean — that is a difficulty cliff, and it is the wave's
fault rather than the player's.

### `economy` — is price even a constraint?

`mean_banked_atp` and `idle_atp_seconds` measure ATP that sat unspent. A large
number on a **won** run means the economy is not a constraint and every price
in `towers.json` is decorative. A large number on a **lost** run means the bot
could not convert money into defence fast enough — usually a placement or
pacing problem, not a pricing one.

### `timeline`

Sampled every 30 ticks (0.5 s): ATP, income, horde size, density, integrity,
tower count. The shape a plot wants.

---

## What makes the numbers trustworthy

- **Attribution is real, not inferred.** Damage in this game is an aggregate
  density field, so "which tower killed that" is not recoverable after the
  fact. `sim/Attribution.h` books every hit against the `owner` the damage
  field or projectile already carried, at the moment it lands, across all three
  damage paths (fields, projectiles, named-agent strikes).
- **Measuring does not change the run.** The sink is null in normal play and
  nothing in the sim ever reads it. `tests/test_autoplay.cpp` asserts that a
  run with the collector attached has a bit-identical `state_hash` to one
  without.
- **Runs are reproducible.** `(level, seed, profile, config_hash)` determines
  the run exactly. The bot uses no RNG and no wall clock.
- **The bot plays the real game.** `game/session/LevelSession.h` holds the one
  authoritative tick order; both `App` and the harness call it. There is no
  second copy to drift.
- **The bot cannot cheat.** Every purchase goes through `Economy::spend` after
  `can_afford`, and every placement through `TowerSystem::validate`.

---

## Turning a finding into a change

Tuning lives in `assets/config/*.json` and is reachable from the console:

```
config set towers.macrophage.2.damage 200
config dump
```

The loop is: sweep → read `summary.md` → change one thing → sweep again →
diff. Change one thing at a time; the harness is precise enough that a single
edit is legible in the next digest, and a batch of edits is not.

Note that `config_hash` changes with the tuning, so reports from before and
after an edit are explicitly labelled as describing different games.
