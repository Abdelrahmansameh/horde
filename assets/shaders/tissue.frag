#version 450 core
// Tissue substrate — fragment stage. Procedural only; this project ships no
// binary art.
//
// ---------------------------------------------------------------------------
// WHAT THIS LAYER IS
// ---------------------------------------------------------------------------
// Everything behind the action: the flesh the level is carved out of, the
// vessel wall, and the plasma inside the lumen the horde swims through. One
// fullscreen quad over the camera's visible extent, reconstructing all of it
// from a signed distance field and two small side textures:
//
//   u_sdf   R32F   signed distance to the vessel wall, world units, + inside.
//                  Normally game/level/RenderSdf.h's field: analytic, with
//                  concave corners already filleted, padded past the level.
//   u_lane  RGBA8  rgb = lane hue, a = lane tempo / 2 (DESIGN.md §9.2).
//
// The flow-field texture the previous treatment advected its plasma along is
// no longer read (the illustration's fluid is still); the renderer keeps the
// texture unit free for it.
//
// ---------------------------------------------------------------------------
// THE LOOK
// ---------------------------------------------------------------------------
// Flat, illustrated, clean -- a vector-art cross-section of tissue, not a wet
// photographic one. NO lighting, NO grain, NO noise-warped edges. Every colour
// and every area fraction below was MEASURED off docs/ref_image.webp rather
// than eyeballed, and the thresholds are set to reproduce those numbers:
//
//   INTERSTITIUM  large DARK cells (a warped Voronoi) separated by a LIGHTER
//                 channel web that pools out where three cells meet. 24.7% of
//                 the area is web, 3.8% is the dark shapes inside the cells,
//                 and the rest is cell body spanning just six luminance
//                 levels -- so nearly all of its structure is carried by hue
//                 and shape, almost none by contrast.
//   WALL          from the fluid outward: a rose lining (lighter, then
//                 darker), then a thin plum line, then the web again.
//   LUMEN         the same construction at a third the scale with the
//                 contrast inverted -- a LIGHT grout with slightly DARKER
//                 rounded cobbles over it, close to half the area each.
//
// Each cell carries up to three crisp shapes -- a big light oval, a dark
// oval, and either a mid-tone oval (sometimes a ring) or a loose cluster of
// small discs -- SIZED AND PLACED TO FIT the cell they belong to, so none of
// them ever needs fading out where it would cross the web.
//
// Every edge is a smoothstep over one pixel's worth of world units, so a wall
// is exactly as smooth as the distance field, and the field is smooth.
//
// SCALE. `u_pattern_scale` is the level's framing height relative to the look's
// reference level (a 143-unit-tall capillary): a level the camera frames from
// four times as far away gets cells, pebbles and a wall four times as large in
// world units, so they take up the same fraction of the screen. It is a
// per-level constant, not the live zoom, so nothing reseeds while panning.

in vec2 v_uv;
in vec2 v_uv_mask;
in vec2 v_world;
in vec2 v_screen;

out vec4 o_color;

layout(binding = 0) uniform sampler2D u_sdf;
layout(binding = 2) uniform sampler2D u_lane;

layout(location = 5) uniform float u_heartbeat_phase;
layout(location = 6) uniform float u_time;
layout(location = 8) uniform float u_have_lane;
layout(location = 11) uniform float u_pattern_scale;

// ===========================================================================
// Palette. Every value below is MEASURED off docs/ref_image.webp -- the mean
// colour of the connected components of each luminance band over a 650x500
// patch of pure interstitium and a 580x120 patch of pure lumen -- not
// eyeballed. The area fractions those measurements produced are quoted where
// they drive a threshold, because they are what the thresholds have to hit.
// ===========================================================================
// Interstitium: DARK cells separated by a LIGHTER channel web (14.4% of area).
const vec3 kCell      = vec3(0.443, 0.106, 0.204);  // #711B34  cell body
const vec3 kCellDark  = vec3(0.435, 0.098, 0.200);  // #6F1933  the darker half
const vec3 kCellLite  = vec3(0.451, 0.110, 0.208);  // #731C35  the lighter half
const vec3 kChannel   = vec3(0.588, 0.157, 0.271);  // #962845  the web
const vec3 kChannelHi = vec3(0.627, 0.176, 0.286);  // #A02D49  its brighter core
const vec3 kDiscLight = vec3(0.596, 0.165, 0.271);  // #982A45  crisp light disc
const vec3 kDiscHi    = vec3(0.608, 0.173, 0.282);  // #9B2C48  its highlight patch
const vec3 kOvalDark  = vec3(0.416, 0.090, 0.188);  // #6A1730  crisp dark oval
const vec3 kOvalMid   = vec3(0.486, 0.118, 0.227);  // #7C1E3A  mid-tone oval
const vec3 kOvalMidHi = vec3(0.518, 0.129, 0.247);  // #84213F  its lighter version
// Wall, from the lumen outward: rose lining, then a thin plum line.
const vec3 kRoseIn    = vec3(0.898, 0.486, 0.522);  // #E57C85
const vec3 kRoseOut   = vec3(0.851, 0.365, 0.459);  // #D95D75
const vec3 kBand      = vec3(0.490, 0.165, 0.314);  // #7D2A50
// Lumen: LIGHTER grout with slightly DARKER cobbles over it, near 50/50.
const vec3 kGrout     = vec3(0.961, 0.592, 0.592);  // #F59797
const vec3 kCobble    = vec3(0.945, 0.541, 0.561);  // #F18A8F
const vec3 kCobbleSat = vec3(0.933, 0.506, 0.553);  // #EE818D  the deeper ones
const vec3 kRim       = vec3(0.965, 0.604, 0.604);  // #F69A9A  rim by the lining

/// The artery hue in render/Renderer.cpp's kLaneVisuals: the palette above is
/// authored for it, and other vessel types are expressed as a ratio to it.
const vec3 kArteryHue = vec3(0.820, 0.227, 0.289);

// ===========================================================================
// Noise toolkit
// ===========================================================================
float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

vec2 hash22(vec2 p) {
    // Two independent lattices: deriving the second channel from the first
    // puts every feature point on one curve through its cell, and a Voronoi
    // built on that grows long slivers that read as scratches.
    return vec2(hash21(p), hash21(p + vec2(71.3, 19.7)));
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i + vec2(0, 0)), hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}

float fbm3(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 3; ++k) {
        v += a * vnoise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return v;
}

/// Jittered-lattice Voronoi, in lattice units.
///
/// `e1` is the exact distance to the nearest cell border and `e2` the distance
/// to the second-nearest. Keeping them apart -- rather than blending them with
/// a smooth-min, which is what an earlier version did -- is what lets the
/// channel web flare at the junctions WITHOUT the cells shrinking: a
/// smooth-min lowers the border distance everywhere two borders are within `k`
/// of each other, and inside a cell that includes the two borders on opposite
/// sides, so the whole cell erodes. `e2 - e1` instead is near zero ONLY where
/// three cells actually meet, and is the cell's full width in its middle.
struct Cells {
    float e1, e2;       ///< nearest / second-nearest border distance
    /// Radius of the largest disc centred on the cell's OWN seed that fits
    /// inside it -- half the distance to the nearest neighbouring seed, which
    /// is exact because a Voronoi border is a perpendicular bisector. It is a
    /// property of the cell, not of the sample point, so every fragment of a
    /// given cell computes the same value; that is what lets the shapes drawn
    /// inside a cell be SIZED TO FIT it instead of being faded out wherever
    /// they happen to cross its border.
    float inradius;
    vec2 id;            ///< winning cell's lattice coordinate, for per-cell hashes
    vec2 to_center;     ///< p -> the winning feature point
};

Cells voronoi(vec2 p, float jitter) {
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    Cells c;
    c.id = vec2(0.0); c.to_center = vec2(0.0);
    float f1 = 8.0;
    vec2 mg = vec2(0.0);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(float(i), float(j));
            vec2 feature = g + 0.5 + (hash22(ip + g) - 0.5) * jitter;
            vec2 r = feature - fp;
            float dd = dot(r, r);
            if (dd < f1) { f1 = dd; c.id = ip + g; c.to_center = r; mg = g; }
        }
    }
    // Distance to each border is the distance to the perpendicular bisector
    // between the winner and that neighbour. Two smallest, kept separately.
    c.e1 = 8.0; c.e2 = 8.0; c.inradius = 8.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 g = mg + vec2(float(i), float(j));
            vec2 feature = g + 0.5 + (hash22(ip + g) - 0.5) * jitter;
            vec2 r = feature - fp;
            // Seed-to-seed offset; its half-length is this border's distance
            // from the cell's own seed, and the smallest of those is the
            // inradius.
            vec2 dr = r - c.to_center;
            float l2 = dot(dr, dr);
            if (l2 < 1e-6) continue;
            float l = sqrt(l2);
            c.inradius = min(c.inradius, l * 0.5);
            float dist = dot(0.5 * (c.to_center + r), dr / l);
            if (dist < c.e1) { c.e2 = c.e1; c.e1 = dist; }
            else if (dist < c.e2) { c.e2 = dist; }
        }
    }
    return c;
}

/// Border distance with the cell's corners FILLETED to radius ~`k`, in
/// lattice units. This is the reference's most characteristic feature: the
/// channel web is three to four times wider where three cells meet than along
/// a plain border, pooling into a smooth curved triangle, and every cell
/// corner is correspondingly rounded off.
///
/// It is the polynomial smooth-min of the two smallest border distances and
/// nothing else. Two earlier versions got this wrong in opposite directions:
/// smooth-minning across ALL the neighbouring borders stacks the reduction
/// once per neighbour and erodes whole cells into islands, while widening the
/// threshold by exp(-(e2-e1)) pulls the channel out into a SPIKE at each
/// junction rather than a pool, because the widening is largest exactly on
/// the ridge where e1 == e2 and falls off to either side of it. Subtracting a
/// smooth-min offset instead moves the whole contour outward by a constant
/// near the junction, which is what a fillet is.
float filleted_edge(Cells c, float k) {
    float h = max(k - (c.e2 - c.e1), 0.0) / k;
    return c.e1 - h * h * k * 0.25;
}

/// Soft-edged disc: 1 inside, 0 outside, `soft` wide transition.
float disc(float dist, float radius, float soft) {
    return 1.0 - smoothstep(radius - soft, radius + soft, dist);
}

// ===========================================================================
// INTERSTITIUM
// ===========================================================================
/// Crisp flat oval: 1 inside, antialiased over `aa` (all in lattice units).
/// `rel` is the vector from the oval's centre, `ang` its orientation,
/// `r` its semi-major radius and `squash` the minor/major ratio.
float oval(vec2 rel, float ang, float r, float squash, float aa) {
    vec2 ax = vec2(cos(ang), sin(ang));
    vec2 loc = vec2(dot(rel, ax), dot(rel, vec2(-ax.y, ax.x)) / squash);
    return 1.0 - smoothstep(r - aa, r + aa, length(loc));
}

vec3 interstitium(vec2 p, float px, float S, float d, float band_w, vec3 tint) {
    // Cell size in world units at the reference framing. The reference frames
    // a 142.8-unit-tall level into 954 px, i.e. 6.68 px per world unit, and
    // its cells measure 220-330 px across -- so 33 to 49 units, call it 36.
    // Two warps: a broad one that shoves whole cells around, and a finer one
    // that bows their sides, so no border is straight and no two cells are
    // the same size.
    float cell = 28.0 * S;
    vec2 warp = (vec2(fbm3(p / (cell * 1.9)), fbm3(p / (cell * 1.9) + 7.3)) - 0.5) * 0.42 +
                (vec2(fbm3(p / (cell * 0.6) + 3.1), fbm3(p / (cell * 0.6) + 11.9)) - 0.5) * 0.12;
    vec2 q = (p + warp * cell) / cell;
    Cells c = voronoi(q, 0.82);
    // Antialiasing width for the flat shapes, in lattice units. `px` is the
    // world size of a pixel, measured once in main() in uniform control flow
    // (a derivative taken inside a branch is undefined, and reads as a blur).
    float aa = px / cell * 1.1 + 0.001;

    // Per-cell identity.
    float h0 = hash21(c.id + 3.1);
    float h1 = hash21(c.id + 11.7);
    float h2 = hash21(c.id + 23.9);
    float h3 = hash21(c.id + 41.3);
    float h4 = hash21(c.id + 57.7);
    float h5 = hash21(c.id + 83.9);

    vec2 rel = -c.to_center;   // from the cell's feature point to p
    // Channel half-width along a plain border. Resolved here rather than at
    // the web, because the shapes inside the cell are fitted against it.
    float w_half = 0.020 + (vnoise(p / (cell * 0.4) + 5.0) - 0.5) * 0.013;

    // ---- The cell body -------------------------------------------------
    // Two things the reference does that a flat fill does not: neighbouring
    // cells sit at visibly different tones (#6D1831 to #731C35, a 6-level
    // spread on a field whose whole range is 20), and WITHIN a cell there is
    // one broad soft lobe at the other tone, which is what makes a cell read
    // as a swollen sac rather than a tile. Both are below the threshold of
    // "a shape" -- they are the ground the crisp shapes are drawn on.
    vec3 col = mix(kCellDark, kCellLite, h0);
    vec2 lobe_off = (hash22(c.id + 19.3) - 0.5) * 0.5;
    float lobe = 1.0 - smoothstep(0.18, 0.52, length((rel - lobe_off) * vec2(1.0, 0.8 + 0.4 * h1)));
    col = mix(col, mix(kCellLite, kCellDark, h0), lobe * 0.6);

    // ---- Crisp shapes inside it ----------------------------------------
    // Measured off the reference: light discs run 30-66 px equivalent
    // diameter and dark ovals 29-52, against a 194-px cell -- so 0.15 to
    // 0.34 of a cell across.
    //
    // EVERY SHAPE IS SIZED AND PLACED TO FIT ITS OWN CELL. `room` is the
    // largest radius that still clears the channel, and a shape of radius `r`
    // is offset by at most `room - r`, so it lies entirely inside the cell by
    // construction. An earlier version instead drew shapes at a fixed size
    // and multiplied them by a "distance from the border" fade, which is what
    // produced the half-dissolved blobs at the cell edges -- a shape that
    // overhung its cell was not moved or shrunk, just ghosted away where it
    // did not belong. It also meant a small cell and a large one carried the
    // same size shapes, so the small ones were mostly fade.
    float room = max(c.inradius - w_half - 0.05, 0.02);
    // Three anchor directions, 120 degrees apart from a per-cell phase, so
    // the shapes spread out instead of piling on one side.
    float phase = h1 * 6.2832;
    vec2 a0 = vec2(cos(phase), sin(phase));
    vec2 a1 = vec2(cos(phase + 2.094), sin(phase + 2.094));
    vec2 a2 = vec2(cos(phase + 4.189), sin(phase + 4.189));

    // Three features, each on its own anchor: a big light oval, a big dark
    // oval, and a third slot that is either a mid-tone oval (sometimes a
    // ring) or a loose cluster of small discs. That is the reference's
    // vocabulary; keeping the cluster OFF the light oval's anchor is what
    // stops the two merging into a snowman.
    //
    // 1. The light oval -- the brightest thing on the interstitium and the
    //    shape the eye reads first.
    if (h0 < 0.95) {
        float r = room * (0.34 + 0.24 * h2);
        vec2 off = a0 * (room - r) * (0.45 + 0.5 * h4);
        float ang = phase + 0.9 + h3 * 1.8;
        float a = oval(rel - off, ang, r, 0.68 + 0.26 * h3, aa);
        col = mix(col, kDiscLight, a);
        // A brighter patch riding on one end of it.
        vec2 ax = vec2(cos(ang), sin(ang));
        float hi = oval(rel - off - ax * r * 0.34, ang, r * 0.46, 0.8, aa) * a;
        col = mix(col, kDiscHi, hi * 0.8);
    }

    // 2. The dark oval, on the opposite side.
    if (h2 > 0.30) {
        float r = room * (0.20 + 0.16 * h4);
        vec2 off = a1 * (room - r) * (0.5 + 0.45 * h5);
        float ang = h3 * 6.2832;
        col = mix(col, kOvalDark, oval(rel - off, ang, r, 0.56 + 0.36 * h5, aa));
    }

    // 3. The third slot.
    if (h1 > 0.42) {
        // A cluster of 2-4 small discs of widely varying size. The reference
        // never draws these as a ring of equal dots, so both the radius and
        // the spacing are drawn per dot.
        vec2 base = a2 * room * (0.42 + 0.35 * h3);
        float rmax = room * (0.10 + 0.06 * h5);
        int n = 2 + int(h4 * 2.99);
        float dots = 0.0;
        for (int k = 0; k < 4; ++k) {
            if (k >= n) break;
            vec2 hk = hash22(c.id + float(k) * 7.7 + 31.0);
            float rr = rmax * (0.35 + 0.65 * hash21(c.id + float(k) + 2.2));
            vec2 dir = normalize(hk - 0.5 + vec2(1e-3));
            vec2 o = base + dir * (rmax * 1.5) * (0.4 + 0.6 * hash21(c.id + float(k) + 5.1));
            // Pull it back inside the cell if it would cross the channel.
            float over = length(o) + rr - room;
            if (over > 0.0) o -= normalize(o) * over;
            dots = max(dots, 1.0 - smoothstep(rr - aa, rr + aa, length(rel - o)));
        }
        col = mix(col, kDiscLight, dots);
    } else if (h3 > 0.2) {
        // A mid-tone oval, sometimes hollow -- the reference has a few
        // distinct rings, and they are what stop the field reading as a
        // repeating two-shape stamp.
        float r = room * (0.24 + 0.22 * h5);
        vec2 off = a2 * (room - r) * (0.5 + 0.45 * h2);
        float ang = h4 * 6.2832;
        float sq = 0.64 + 0.3 * h2;
        float a = oval(rel - off, ang, r, sq, aa);
        if (h5 > 0.7) a *= 1.0 - oval(rel - off, ang, r * 0.5, sq, aa);
        col = mix(col, mix(kOvalMid, kOvalMidHi, h2), a);
    }

    // ---- The channel web -----------------------------------------------
    // Flat and crisp, widening into a smooth pool at every junction; see
    // filleted_edge(), which is where that comes from. Its half-width was
    // resolved above, before the shapes, because they are fitted against it.
    float e = filleted_edge(c, 0.35);
    float chan = 1.0 - smoothstep(w_half - aa, w_half + aa, e);
    // A slightly brighter thread down the middle of the web.
    float core = 1.0 - smoothstep(0.25 * w_half, w_half, e);
    // A soft skirt of the channel's colour spilling a little way into the
    // cell. The reference's cell does not butt against the web: there is a
    // 15-px ramp between them, which is most of its mid-tone band.
    float skirt = 1.0 - smoothstep(w_half, w_half + 0.060, e);
    col = mix(col, kChannel, skirt * 0.30);
    col = mix(col, mix(kChannel, kChannelHi, core * 0.3), chan);

    // The same web runs along the outside of every vessel, its width
    // wandering from nothing to a wide pool.
    float wall_dist = -d - band_w;   // 0 at the plum line's outer edge
    float pool = -0.5 + 5.0 * fbm3(p / (cell * 0.45) + 21.0);
    float along = 1.0 - smoothstep(max(pool, 0.0) * S - px, max(pool, 0.05) * S + px, wall_dist);
    col = mix(col, kChannel, along);

    return col * tint;
}

// ===========================================================================
// LUMEN
// ===========================================================================
vec3 lumen(vec2 p, float px, float S, float d, vec3 tint) {
    // The fluid is the same construction as the interstitium at a quarter the
    // scale and with the contrast inverted: a LIGHT grout with slightly
    // DARKER cobbles over it, close to half the area each (measured 40.4%
    // grout / 40.8% cobble). The cobbles' equivalent diameters run 38-87 px
    // at 6.68 px per world unit, so 5.7 to 13 units across; a jittered
    // lattice of this pitch spans that range.
    float cell = 12.0 * S;
    vec2 warp = (vec2(fbm3(p / (cell * 2.2) + 41.0), fbm3(p / (cell * 2.2) + 57.0)) - 0.5) * 0.5;
    vec2 q = (p + warp * cell) / cell;
    Cells c = voronoi(q, 1.0);
    float aa = px / cell * 1.1 + 0.001;

    float h0 = hash21(c.id + 5.9);
    float h1 = hash21(c.id + 13.3);

    vec3 col = kGrout;
    // Grout half-width: the gap between two cobbles is about twice this. It
    // varies PER COBBLE, not just from place to place, which is what gives
    // the reference's wide spread of cobble sizes -- a cobble that keeps more
    // grout around it is simply a smaller cobble. A shared width would tile
    // the lane with one size.
    float h2 = hash21(c.id + 31.7);
    float w = (0.030 + 0.055 * h2 * h2) + (vnoise(p / (cell * 0.8) + 3.0) - 0.5) * 0.014;
    // A large fillet: the reference's cobbles are rounded blobs with generous
    // gaps, not a polygonal tiling with thin grout.
    float e = filleted_edge(c, 0.55);
    float cobble = smoothstep(w - aa, w + aa, e);
    // A sixth of them sit deeper, which is what keeps the fluid from reading
    // as a regular tiling.
    col = mix(col, (h0 > 0.84) ? kCobbleSat : kCobble, cobble);
    // And a broad soft lobe inside some cobbles, as in the interstitium.
    if (h1 > 0.5) {
        vec2 off = (hash22(c.id + 23.1) - 0.5) * 0.4;
        float lobe = 1.0 - smoothstep(0.16, 0.44, length(-c.to_center - off));
        col = mix(col, kCobbleSat, lobe * cobble * 0.5);
    }

    // The lining: a rose band against the wall, darker on the outside, with
    // a lighter rim of fluid just inside it.
    float lining_w = 2.3 * S;
    float aad = px * 0.7;
    float in_lining = 1.0 - smoothstep(lining_w - aad, lining_w + aad, d);
    float rim = (1.0 - smoothstep(lining_w, lining_w + 2.0 * S, d)) * (1.0 - in_lining);
    col = mix(col, kRim, rim * 0.7);
    vec3 rose = mix(kRoseIn, kRoseOut, smoothstep(0.7 * lining_w, 0.35 * lining_w, d));
    col = mix(col, rose, in_lining);

    // Resting pulse on the whole lumen; the sim's heartbeat phase drives it.
    col *= 1.0 + 0.012 * sin(u_heartbeat_phase);
    return col * tint;
}

// ===========================================================================
void main() {
    vec2 uv = clamp(v_uv, 0.0, 1.0);
    vec2 p = v_world;
    float S = max(u_pattern_scale, 0.05);

    float d = texture(u_sdf, uv).r;

    // ---- Lane identity ---------------------------------------------------
    // The palette is authored for an artery; other vessel types tint it by
    // their hue's ratio to the artery hue, held well short of 1:1 so the
    // illustration's values survive.
    vec3 lumen_tint = vec3(1.0);
    vec3 wall_tint = vec3(1.0);
    vec3 flesh_tint = vec3(1.0);
    if (u_have_lane > 0.5) {
        vec3 hue = texture(u_lane, clamp(v_uv_mask, 0.0, 1.0)).rgb;
        vec3 ratio = clamp(hue / kArteryHue, 0.35, 2.2);
        lumen_tint = mix(vec3(1.0), ratio, 0.32);
        wall_tint = mix(vec3(1.0), ratio, 0.22);
        flesh_tint = mix(vec3(1.0), ratio, 0.06);
    }

    // ---- Bands ------------------------------------------------------------
    // Everything in world units, scaled with the framing. The plum line sits
    // just outside the wall (d < 0); the rose lining is the lumen's own edge
    // (drawn inside lumen()), and the mortar channel outside the line is the
    // interstitium's business.
    const float kBandThick = 1.3;
    float band_w = kBandThick * S;

    // World size of one pixel, from the position's screen-space derivative;
    // every antialiasing width below derives from it. Taken here, in uniform
    // control flow, because a derivative inside a branch is undefined.
    float px = max(length(fwidth(p)) * 0.7071, 1e-4);
    float aa = px * 0.7;

    float inside = smoothstep(-aa, aa, d);
    float in_band = smoothstep(-band_w - aa, -band_w + aa, d);

    vec3 col;
    if (inside > 0.999) {
        col = lumen(p, px, S, d, lumen_tint);
    } else {
        col = interstitium(p, px, S, d, band_w, flesh_tint);
        col = mix(col, kBand * wall_tint, in_band);
        if (inside > 0.001) col = mix(col, lumen(p, px, S, d, lumen_tint), inside);
    }

    // Static dither so the flat gradients cannot band in 8-bit.
    col += (hash21(gl_FragCoord.xy) - 0.5) * 0.006;

    o_color = vec4(max(col, vec3(0.0)), 1.0);
}
