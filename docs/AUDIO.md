# Audio

[Documentation index](README.md) · [Architecture](ARCHITECTURE.md) · [Rendering](RENDERING.md#combat-events-and-cosmetic-particles)

Every sound is synthesized at runtime; the repository ships no audio files.
Audio is cosmetic like particles: it reads the same drained combat-event span
and a few counters, owns its own RNG, and never feeds anything back. Muting it,
running without a device, or dropping sounds cannot change a run.

## Layers

| Layer | Source | Thread | Job |
|---|---|---|---|
| Scene | [SoundScape](../src/audio/SoundScape.h) | game | Turns a frame of events, cues and horde counts into a bounded set of commands |
| Queue | `CommandRing` in [Mixer.h](../src/audio/Mixer.h) | both | Lock-free SPSC ring, 2048 commands; full = drop the newest and count it |
| Mixer | [Mixer](../src/audio/Mixer.h) | audio | Voices, grains, beds, buses, room reverb, compressor, soft clip |
| Sounds | [Patches](../src/audio/Patches.h) | — | The sound design as data: voice patches, grain shapes, beds |
| Device | [AudioEngine](../src/audio/AudioEngine.h) | audio | SDL device; calls the mixer from SDL's callback |

`immune_audio` depends on core and sim (event types only). `App` owns one
`AudioEngine` and one `SoundScape`. The mixer and scene are SDL-free, so tests
and the offline renderer drive exactly the code the game runs.

## What makes sound

| Group | Game source | Sound |
|---|---|---|
| UI | gui pointer state machine (`gui::UiSound`: hover, click, deny) | Soft tick, two-partial blip, falling two-note "uh-uh" |
| UI | pause open/close, Strengthen Immunity purchase/failure | Rising/falling bell pair, four-note arpeggio, deny |
| Player actions | deploy (per cell type, on its own note), ability cast, failed deploy/cast | Plop + bell; sparkle run, whoosh, thud; deny |
| Run | wave start, wave cleared, victory, defeat, leak into the objective | Horn swell, bell figure, arpeggio + swell, falling lament, low thump + warning tone |
| Friendly attacks | `MuzzleFlash`, `ProjectileImpact` (by source), `Explosion`, `ScarBuilt/Reinforced`, legacy beam/chain/cone/blade | "Pew", tick grain, gulp, stab, splat, woody knock, zap |
| Friendly deaths | `SwarmerDeath` with cause Damage, `TowerDestroyed`, `ScarDestroyed` (chewed through) | Falling, darkening "deflate"; boom; crunch |
| Horde attacks | `PathogenLatch`, `ToxinFired`, `ToxinHit`, `HarpoonPulse` (bite / slug), `ScarChipped` | Squelch, spit, acid hiss, twanging thwip, glug, chew |
| Horde deaths | `ChaffDeath` by family | Grains: virus bubble (rising), bacteria pop, parasite squelch, spore puff |
| Beds | visible pathogens, `chaff_latched`, death rate | Breathing murmur, chewing texture, fizz |

`ToxinFired`/`ToxinHit` exist for audio (the pellet itself is drawn from live
state, so the particle layer ignores them); `HostileTuning::max_toxin_events`
caps them per tick. `CombatEvent::cause` carries a `CombatRetirement` for
`SwarmerDeath`: only Damage (or a hand-raised Unknown) plays the loss sound —
a detonating bomber or a builder spent on its wall already sounds as that
attack, and a released unit ageing out is not a loss.

Silent on purpose: `ProjectileExpired`, `Freeze`, `Shatter`, `FluidSplash` and an
expired scar. The test "every combat event type is heard or deliberately
silent" fails when a new event type is added without a decision.

## Readability and tuning rules

- Friendly sounds are clean and tonal (sine, triangle, bells); the horde is
  noisy and organic (filtered noise, squelch, buzz). Pathogen deaths are soft
  reward pops; friendly deaths slide down and darken. Alerts sit in the low
  register, where nothing in combat lives, and duck the combat buses.
- Every pitched sound is in D major pentatonic (`degree_semitones()`), so any
  pile-up of notes from different sources stays consonant.
- Repeats vary: random scale degrees, small detune, random oscillator phases,
  sub-frame onset jitter. There is no "machine gun" of identical copies.
- Buses: UI and Alert are never ducked; an Alert voice ducks Friendly and
  Horde by about 3.7 dB, released over ~0.35 s.

## The horde strategy

A busy wave raises hundreds of events per tick. The scene never plays one
sound per event; each frame it:

1. **Aggregates.** Events are binned per category (16 categories plus one
   death tally per family). Each bin keeps a count and up to 4 (deaths: 8)
   reservoir-sampled positions, so the voices it does play are spread
   fairly over where things happened.
2. **Rate-limits.** Each category has a token bucket (sustained rate, burst,
   per-frame cap; table in `SoundScape.cpp`). The frame also has global budgets:
   12 new voices and 16 new grains. Unvoiced events are not lost: they raise
   the loudness of the voices that play by a fixed amount per doubling,
   capped (latch: +0.25 per doubling, at most ×1.6 ≈ +4 dB for 256 at once).
   Loudness grows logarithmically with the crowd.
3. **Culls by audibility.** Positions are judged against the camera's visible
   rectangle: full volume on screen with slight centre emphasis, fading to
   silence 40% of a view beyond the edge. Pan follows screen x (±0.7). A few
   categories keep a floor wherever they happen (a lost tower: 0.5).
4. **Switches to texture.** Pathogen deaths share one grain bucket (110/s,
   burst 12). As the smoothed audible death rate climbs past 12/s each pop
   gets quieter (rate^-0.3, floor 0.45) and the fizz bed fades in between 80
   and 1500 deaths/s: individual pops in a skirmish, a hiss in a slaughter.
   The murmur bed follows the pathogens near the view (log scale to ~3000)
   and the chewing bed follows how many are feeding on friendlies.
5. **Protects what matters.** Leaks are checked first, outside the budgets
   (2.5/s, burst 2). The mixer's voice pool steals the quietest voice of the
   lowest priority and never a Protected one (objective thump, tower lost).

Beds run only while the sim is advancing (paused, 0× or editing fades them).

## Mixer

| Resource | Size | Notes |
|---|---|---|
| Voices | 64 | 2 oscillators (+phase modulation), noise, TPT state-variable filter, attack/hold/exp-decay, pitch and cutoff envelopes |
| Grains | 96 | One sine with exponential glide + filtered noise transient; about a third of a voice |
| Beds | 3 | Murmur, Gnaw, Fizz; target level smoothed per bed |
| Room | 4+4 combs, 2+2 allpasses | Freeverb tuning, small and dark |
| Master | — | Smoothed volume → peak compressor (−6 dBFS, 4:1) → soft clipper |

Control-rate work (pitch, cutoff, FM depth) runs every 16 samples. Each patch
caps its own live instances (stealing its oldest). The callback flushes
denormals while rendering, never allocates or locks, and refuses non-finite
command values. `StopAll` (sent by `SoundScape::reset()` on every level load
and return to the menus) fades everything within a few milliseconds.

## Using it

- `--volume F` (0–1, default 0.8) and `--no-audio` on the command line; **M**
  toggles mute in game.
- Gym console: `audio` / `audio stats` (device, voices, load, scene counters),
  `audio volume <0-1>`, `audio mute|unmute`, `audio cue <name> [variant]`
  (`audio cue list`).
- On exit the log records one `audio:` line: events in, voices and grains
  out, culled and folded counts, mixer load, steals and ring drops.
- No device (headless box, remote desktop) logs `audio disabled: …` and the
  game runs silent.

## Auditioning and measuring

```powershell
& $immuneExe --audio-render scratch/audio/audition.wav
```

Renders a 52-second timeline through the real scene and mixer at 48 kHz —
UI cues, deploy per cell type, abilities, each friendly attack, friendly
deaths, each horde attack, horde deaths per family plus a 20-death burst,
the run stingers, a 10-second escalating horde (5 → 3000 deaths/s, 10 → 600
shots/s, up to 8000 visible pathogens and 300 feeding), and a tail — and
prints JSON with peak/RMS dBFS, render load, voice/grain counts per segment.
Listen to the WAV when changing a patch: tests cannot judge how it sounds.

Measured 2026-10-27 on the development machine (windows-release build, one
thread): the whole timeline rendered at 0.35% of one core; the horde stress
segment averaged 1.1% with a worst 13 ms frame at 2.1%. Zero clipped samples,
zero non-finite samples, zero ring drops; 8116 stress events became 303 voices
and 968 grains. Peak −3.1 dBFS, RMS −21.6 dBFS in the stress segment.

## Tests

[test_audio.cpp](../tests/test_audio.cpp) (`[audio]`): ring FIFO/overflow;
silence when idle; every patch and grain renders audible, finite, bounded and
ends; per-patch caps; Protected voices survive a full pool and low priorities
are rejected; pan law; the limiter holds 64 loud voices under full scale;
non-finite commands refused; delays; beds rise and release; StopAll; the scene's
budgets for 1000 deaths in a frame, sustained-rate limits and the fizz
crossover, off-screen culling with the tower-lost floor, pan, death causes,
logarithmic loudness, leak thumps, UI rate limits, beds pausing, event-type
coverage, presence sampling; and the full audition (no clipping, every
segment audible, render load well under budget). The gui's sound queue
(`Gui::sounds()`) has no dedicated test yet.

## Limitations

- Sound quality is judged by ear; the audition WAV is the review artefact.
- Presence and the chewing bed are crowd-level, not per-cell positional.
- There is no music, no settings screen for volume (CLI, `M` and the gym
  only), and no separate SFX/UI sliders.
- Off-screen culling uses the camera rectangle only; zoom does not change
  per-event loudness.
