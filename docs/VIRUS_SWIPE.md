# Main-menu virus swipe

Clicking **Play** on the main menu sends a dense, full-height horde of gameplay
viruses across the screen. The GUI passes screen-space geometry to the actual
`chaff.frag` gameplay shader, sharing its live virus color and animation tuning.
There is no solid backdrop: the menu shows through gaps between viruses.
By default the swarm enters from the left, spans the screen, switches to the next
screen at peak coverage, then continues right to reveal it. The screen swaps
instantly at the crossover, without a trailing fade. Viruses retain forward
motion into and out of a brief 15 ms covered pause.

Edit `assets/config/virus_swipe.json` to tune the effect. The file is read again
when Play is clicked, so loose-asset development builds do not need a restart.
Standalone embedded builds use the copy packaged into their executable.

| Key | Default | Allowed values and behavior |
| --- | --- | --- |
| `enabled` | `true` | Boolean. `false` opens the next screen immediately. |
| `virus_count` | `24000` | Whole number, 1–50000. Exact number of virus sprites in the horde. |
| `density` | `1.0` | 0.1–4. Raises or lowers overlap by multiplying sprite sizes by `sqrt(density)`; count stays the same. |
| `duration_seconds` | `1.095` | 0.1–10 seconds. Total effect duration, including the covered hold. |
| `cover_fraction` | `0.52` | 0.1–0.9. Portion of the moving time used to cover the menu; the rest reveals the next screen. |
| `hold_seconds` | `0.015` | 0 up to half `duration_seconds`. Optional covered pause; zero keeps motion continuous. |
| `switch_fraction` | `0.5` | 0–1. Manual screen swap within an optional covered hold: 0 starts it, 0.5 is its midpoint, 1 ends it. Default swap time is 0.5691 seconds after Play. |
| `min_size` | `32` | 12–300 logical UI pixels, before the density multiplier. Smallest sprite size. |
| `max_size` | `70` | 12–300 logical UI pixels and at least `min_size`. Largest sprite size. |
| `edge_irregularity` | `100` | 0–400 logical UI pixels. Variation along the advancing horde edge. |
| `drift` | `22` | 0–100 logical UI pixels. Amount of individual virus movement. |
| `stagger_seconds` | `0.14` | 0–1 second. Random entry delays and exit acceleration variation. Capped at 65% of each pass. Exiting viruses keep moving through the crossover. |
| `speed_variation` | `0.35` | 0–0.8. Random speed-curve variation. Viruses reach the crossover at the authored time instead of arriving early and stopping. |
| `path_spread` | `90` | 0–300 logical UI pixels. Individual curved flight paths; zero removes this sideways wandering. |
| `spin` | `1.8` | −12–12 radians per second. Virus rotation amount; zero stops rotation. |
| `seed` | `1337` | Whole number, 0–4294967295. Reproducible random arrangement. Change it for a different horde. |
| `from_left` | `true` | Boolean. `true` sweeps left to right; `false` reverses the direction. |
| `tint` | `"#FFFFFF"` | Hex color multiplied into the gameplay virus color; white preserves its original colors. Alpha is allowed. |

Color strings accept `#RGB`, `#RRGGBB`, `RRGGBB`, or `#RRGGBBAA`. Sparse count or
density settings leave more of the screens visible through the swarm. The old
`icon` and `background` settings have been removed.

All keys are required. Unknown keys, malformed JSON, incorrect types, nonfinite
numbers, and values outside the ranges above reject the entire update; the
loader leaves the supplied settings unchanged and reports the file/field error.
Keep all keys when experimenting. Increasing both `virus_count` and `density`
creates a heavier swarm; reducing `duration_seconds` makes the swipe faster.

For a reproducible video capture, build the tests and run the opt-in GL fixture:

```powershell
$env:IMMUNE_SWIPE_CAPTURE_DIR = "$PWD/scratch/virus-swipe/frames"
& "$env:LOCALAPPDATA/horde-build/windows-release/bin/immune_tests.exe" '[virus_swipe_capture]'
ffmpeg -framerate 60 -i scratch/virus-swipe/frames/frame_%04d.png -c:v libx264 -crf 18 -pix_fmt yuv420p -movflags +faststart scratch/virus-swipe/virus-swipe.mp4
```

The fixture renders the actual menu and immunity screen through the game GUI
and OpenGL renderer, triggers Play after one second, and records four seconds.
It needs an OpenGL 4.5 context but no desktop input or visible window.
