"""IMMUNE out-of-match flow: main menu, Strengthen Immunity tree, level select, results."""
import glob
import json
import math
import os
import random
import re

from hud_screens import *  # noqa: F401,F403  (palette, cell panels, illustrations, world pieces)

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


# ------------------------------------------------------------------ page with logic
def page_js(title, body, js, w=W, h=H, bg=MATRIX):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="./support.js"></script>
</head>
<body>
<x-dc>
{HELMET.replace("</style>", ".anim-flow{animation:flow 1.8s linear infinite}@keyframes flow{to{stroke-dashoffset:-44}}</style>")}
<div style="position: relative; width: {w}px; height: {h}px; overflow: hidden; background: {bg}; font-family: {TEXTF}; color: {INK}">
{body}
</div>
</x-dc>
<script type="text/x-dc" data-dc-script data-props='{{"$preview":{{"width":{w},"height":{h}}}}}'>
{js}
</script>
</body>
</html>
"""


EMPTY_JS = "class Component extends DCLogic {\nrenderVals() {\nreturn {};\n}\n}"


def link_cell(href, x, y, w, h, inner, fill=LIL_DEEP, rim=LIL_RIM, seed=3, color="#fff", aria="", extra="", n_exp=3.0, rimw=6, cls=""):
    c = f' class="{cls}"' if cls else ""
    return (f'<a{c} href="{href}" aria-label="{aria}" style="position: absolute; left: {x}px; top: {y}px; width: {w}px; height: {h}px; display: block; '
            f'text-decoration: none; color: {color}; cursor: pointer; {extra}">'
            f'{membrane_svg(w, h, fill, rim, seed, rimw, 2.4, n_exp, organelles=False)}'
            f'<span style="position: relative; display: flex; align-items: center; justify-content: center; gap: 12px; width: {w}px; height: {h}px">{inner}</span></a>')


def dim(alpha):
    return f'<div style="position: absolute; inset: 0; background: #1E0010; opacity: {alpha}"></div>'


def backdrop(content="", alpha=0.0):
    return (f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" style="position: absolute; left: 0; top: 0" aria-hidden="true">'
            f'<defs><filter id="soft" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="9"/></filter></defs>'
            f'{tissue()}{content}</svg>' + (dim(alpha) if alpha else ""))


def pill(text, bg=PLUM, fg="#fff", size=14, extra=""):
    return (f'<span style="display: inline-flex; align-items: center; gap: 6px; padding: 4px 12px; border-radius: 999px; background: {bg}; color: {fg}; '
            f'font-family: {TEXTF}; font-size: {size}px; font-weight: 900; white-space: nowrap; {extra}">{text}</span>')


# ================================================================== MAIN MENU
def macrophage_hero(x, y, s):
    """The title-screen mascot: a macrophage reaching for a bacterium."""
    pts = [(40, 150), (14, 110), (40, 76), (84, 80), (104, 30), (150, 18), (176, 52), (160, 92), (214, 96), (260, 76), (296, 104),
           (262, 140), (236, 186), (250, 236), (200, 262), (150, 240), (104, 270), (54, 244), (58, 196)]
    body = smooth_closed(pts)
    return (f'<g transform="translate({x} {y}) scale({s})">'
            f'<ellipse cx="150" cy="286" rx="130" ry="20" fill="#1E0010" fill-opacity="0.35"/>'
            f'<path d="{body}" fill="{BODY}" stroke="{PLUM}" stroke-width="7"/>'
            f'<path d="{smooth_closed(blob_pts(120, 110, 50, 30, 4, 3, 2, 24))}" fill="{BODY_HI}" fill-opacity="0.6"/>'
            f'<path d="M110 170c0-40 40-58 70-40 22 12 12 38-8 38-24 0-18 30-8 44-26 10-54-10-54-42Z" fill="{NUC}"/>'
                        f'<circle cx="210" cy="200" r="30" fill="#fff" fill-opacity="0.35" stroke="{LIL_DEEP}" stroke-width="4"/>'
            f'{bacteria_inner(210, 200, 0.8, 30)}'
            f'<path d="M290 104c30-4 58 6 78 30" stroke="{PLUM}" stroke-width="24" fill="none" stroke-linecap="round"/>'
            f'<path d="M290 104c30-4 58 6 78 30" stroke="{BODY}" stroke-width="15" fill="none" stroke-linecap="round"/>'
            f'<path d="M368 134c10 4 16 12 18 22M368 134c14-2 24 2 30 10" stroke="{PLUM}" stroke-width="12" fill="none" stroke-linecap="round"/>'
            f'<path d="M368 134c10 4 16 12 18 22M368 134c14-2 24 2 30 10" stroke="{BODY}" stroke-width="6" fill="none" stroke-linecap="round"/>'
            f'<g class="anim-wobble" style="transform-box: fill-box; transform-origin: center">{bacteria_inner(430, 176, 1.9, -35)}</g></g>')


def scene_menu():
    rnd = random.Random(4)
    floaters = ""
    for i in range(16):
        x, y = rnd.uniform(1000, 1880), rnd.uniform(60, 1020)
        f = rnd.choice(["b", "v", "v", "b", "p"])
        floaters += (bacteria_inner(x, y, rnd.uniform(0.5, 0.9), rnd.uniform(0, 180)) if f == "b" else
                     virus_inner(x, y, rnd.uniform(0.5, 0.8)) if f == "v" else parasite_inner(x, y, 0.8))
    vp = "M1050 1120C1120 820 1500 760 1560 520S1760 180 2000 120"
    vessel = (f'<path d="{vp}" stroke="{LANE_RIM}" stroke-width="210" fill="none"/><path d="{vp}" stroke="{LANE_WALL}" stroke-width="190" fill="none"/>'
              f'<path d="{vp}" stroke="{LANE_IN}" stroke-width="160" fill="none"/><path d="{vp}" stroke="{LANE_CORE}" stroke-width="70" stroke-opacity="0.8" fill="none"/>')
    bg = backdrop(vessel + floaters + macrophage_hero(1080, 360, 1.25))
    logo = (f'<div style="position: absolute; left: 150px; top: 200px; font-family: {DISPLAY}; font-weight: 700; font-size: 188px; line-height: 0.95; '
            f'letter-spacing: 0.02em; color: {BODY}; -webkit-text-stroke: 9px {PLUM}; paint-order: stroke fill; text-shadow: 0 10px 0 rgba(30,0,16,0.45)">IMMUNE</div>')
    play = link_cell("Tree.dc.html", 160, 520, 460, 120,
                     f'<svg width="40" height="40" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4.5v15L19.5 12Z" fill="#fff" stroke="{PLUM}" stroke-width="2" stroke-linejoin="round"/></svg>'
                     f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 46px; color: #fff; text-shadow: 0 3px 0 {PLUM}">Play Game</span>',
                     LIL_DEEP, LIL_RIM, 12, aria="Play Game", rimw=9)
    quit_ = (f'<button aria-label="Quit to Desktop" style="position: absolute; left: 190px; top: 672px; width: 400px; height: 86px; padding: 0; border: none; background: transparent; cursor: pointer">'
             f'{membrane_svg(400, 86, FL_FILL, FL_RIM, 13, 6, 2.2, 3, organelles=False)}'
             f'<span style="position: relative; display: flex; align-items: center; justify-content: center; height: 86px; font-family: {DISPLAY}; font-weight: 600; font-size: 30px; color: {INK}">'
             f'Quit to Desktop</span></button>')
    return page_js("IMMUNE main menu", bg + logo + play + quit_, EMPTY_JS)


# ================================================================== SKILL TREE
def parse_tree():
    src = open(os.path.join(REPO, "src", "game", "meta", "ImmunityTree.cpp"), encoding="utf-8").read()
    rx = re.compile(r'node\("([^"]+)", "([^"]+)", "([^"]+)", K::(\w+), B::(\w+), (\d+)(?:, (\d+))?(?:, (\w+))?\)')
    nodes = []
    for m in rx.finditer(src):
        nid, name, desc, kind, branch, mx, w, _ab = m.groups()
        nodes.append(dict(id=nid, k=nid.replace(".", "_"), name=name, desc=desc, kind=kind, branch=branch, max=int(mx), w=int(w or 100)))
    return nodes


GLYPH_RULES = [
    ("health", ["scar_health", "membrane", "resilience", "homeostasis", "health", "vitality", "body_mass"]),
    ("atp", ["reserve", "clearance", "requisition", "deployment", "metabolism"]),
    ("time", ["duration", "stamina", "slow_strength"]),
    ("wall", ["wall", "scar_size", "width"]),
    ("fire", ["weakness", "inflammation"]),
    ("damage", ["damage", "drain", "potency", "elite", "magnitude"]),
    ("cadence", ["cadence", "trigger", "cooldown", "reinforce", "grab_speed", "attach"]),
    ("reach", ["range", "search", "radius", "splash"]),
    ("count", ["squad", "droplets", "arms", "captives", "max_scars", "body_count", "chain"]),
    ("aim", ["accuracy"]),
]
CAP_GLYPH = {"neutrophil": "fire", "cytotoxic": "damage", "macrophage": "health", "goblet": "time", "fibroblast": "fire"}
TOWER_OF = {"Neutrophil": "neutrophil", "CytotoxicT": "cytotoxic_t", "Macrophage": "macrophage", "GobletCell": "goblet_cell", "Fibroblast": "fibroblast"}
ABIL_OF = {"complement": "cascade", "histamine": "histamine", "fever": "fever", "clot": "clot"}
BR_NAME = {"Hub": "Core immunity", "Neutrophil": "Neutrophil", "CytotoxicT": "Cytotoxic T", "Macrophage": "Macrophage", "GobletCell": "Goblet Cell", "Fibroblast": "Fibroblast"}


def glyph_of(n):
    if n["kind"] == "TowerRoot":
        return "t_" + TOWER_OF[n["branch"]]
    if n["kind"] == "AbilityRoot":
        return "a_" + ABIL_OF[n["id"].split(".")[1]]
    if n["kind"] == "Capstone":
        return CAP_GLYPH[n["id"].split(".")[0]]
    tail = n["id"].split(".")[-1] if n["kind"] != "Economy" else n["id"].split(".")[1]
    for g, keys in GLYPH_RULES:
        if any(kk in tail for kk in keys):
            return g
    return "damage"


def glyph_svg(g, s=1.0):
    """Stat glyphs in a 0-centred space, radius ~26."""
    P, I = PLUM, INK
    if g.startswith("t_"):
        return f'<g transform="scale({s * 1.02}) translate(-32 -32)">{tower_inner(g[2:])}</g>'
    if g.startswith("a_"):
        return f'<g transform="scale({s * 0.86}) translate(-32 -32)">{ability_inner(g[2:])}</g>'
    body = {
        "damage": f'<path d="M-18 -16h36l-9 36-9-15-9 15Z" fill="#fff" stroke="{I}" stroke-width="5"/><path d="M-8 -16v8M8 -16v8" stroke="{I}" stroke-width="3"/>',
        "cadence": f'<circle r="21" fill="#fff" stroke="{I}" stroke-width="5"/><path d="M0 -11V0l8 6" stroke="{I}" stroke-width="5" fill="none" stroke-linecap="round"/><path d="M-27 -6a28 28 0 0 1 8-15" stroke="{I}" stroke-width="4" fill="none" stroke-linecap="round"/>',
        "reach": f'<circle r="25" fill="none" stroke="{I}" stroke-width="4" stroke-dasharray="5 6"/><circle r="15" fill="none" stroke="{I}" stroke-width="4"/><circle r="6" fill="{NUC}"/>',
        "count": f'<circle cx="-11" cy="8" r="10" fill="{BODY_HI}" stroke="{I}" stroke-width="4"/><circle cx="11" cy="8" r="10" fill="{BODY_HI}" stroke="{I}" stroke-width="4"/><circle cx="0" cy="-11" r="10" fill="{BODY_HI}" stroke="{I}" stroke-width="4"/>',
        "aim": f'<circle r="17" fill="#fff" stroke="{I}" stroke-width="5"/><circle r="5" fill="{RED}"/><path d="M0 -27v10M0 17v10M-27 0h10M17 0h10" stroke="{I}" stroke-width="5" stroke-linecap="round"/>',
        "health": f'<path d="M-8 -22h16v14h14v16H8v14H-8V8h-14V-8h14Z" fill="#F08A9C" stroke="{I}" stroke-width="5" stroke-linejoin="round"/>',
        "time": f'<path d="M-15 -22h30M-15 22h30M-12 -22c0 14 24 16 24 44M12 -22c0 14-24 16-24 44" stroke="{I}" stroke-width="5" fill="none" stroke-linecap="round"/><path d="M-7 15 0 8l7 7Z" fill="#CFE8FF" stroke="{I}" stroke-width="3"/>',
        "atp": mito_inner(0, 0, 0.7, -20),
        "wall": "".join(f'<rect x="{x}" y="{y}" width="20" height="12" rx="3" fill="#F28AA0" stroke="{I}" stroke-width="4"/>' for x, y in [(-22, -18), (2, -18), (-12, -4), (-22, 10), (2, 10)]),
        "fire": f'<path d="M0 -26c10 12 20 20 16 34a17 17 0 0 1-32 0c-2-9 4-14 8-18 0 8 4 11 7 11C1 -6-4-14 0-26Z" fill="#F26A3D" stroke="{I}" stroke-width="5" stroke-linejoin="round"/><path d="M0 6c4 4 6 7 4 11a5 5 0 0 1-9 0c0-4 3-6 5-11Z" fill="{GOLD}"/>',
    }[g]
    return f'<g transform="scale({s})">{body}</g>'


WS = 2800          # the web's world is WS x WS, hub at its centre
WC = WS // 2
SPOKES = {"Neutrophil": -162, "CytotoxicT": -126, "Macrophage": -90, "GobletCell": -54, "Fibroblast": -18,
          "clot": 18, "fever": 54, "Core": 90, "histamine": 126, "complement": 162}
ROOT_R = 330
MINI = 240


def polar(ang, r, side=0.0):
    a = math.radians(ang)
    return (WC + r * math.cos(a) - side * math.sin(a), WC + r * math.sin(a) + side * math.cos(a))


def fan(nodes_, ang, r0, step=140, off=82):
    """Pairs either side of the spoke, one ring per pair; an odd last one sits on the spoke."""
    out = []
    for j, n in enumerate(nodes_):
        row, col = divmod(j, 2)
        single = j == len(nodes_) - 1 and len(nodes_) % 2 == 1
        r = r0 + step * row
        out.append((n, polar(ang, r, 0 if single else (-off if col == 0 else off)), r))
    return out


def tree_layout(nodes):
    pos, size, spoke_end = {}, {}, {}
    by_branch = {}
    for n in nodes:
        by_branch.setdefault(n["branch"], []).append(n)
    eco = [n for n in by_branch["Hub"] if n["kind"] == "Economy"]
    for n, pnt, r in fan(eco, SPOKES["Core"], 300):
        pos[n["k"]], size[n["k"]] = pnt, 60
        n["axis"], n["r"] = SPOKES["Core"], r
    spoke_end["Core"] = 300 + 140 * 4
    for key in ("complement", "histamine", "fever", "clot"):
        ang = SPOKES[key]
        grp = [n for n in by_branch["Hub"] if n["id"].startswith(f"ability.{key}.")]
        root = [n for n in grp if n["kind"] == "AbilityRoot"][0]
        pos[root["k"]], size[root["k"]] = polar(ang, ROOT_R), 96
        root["axis"], root["r"] = ang, ROOT_R
        for n, pnt, r in fan([n for n in grp if n["kind"] != "AbilityRoot"], ang, 480):
            pos[n["k"]], size[n["k"]] = pnt, 64
            n["root"], n["axis"], n["r"] = root["k"], ang, r
        spoke_end[key] = (root["k"], 620)
    for br in ("Neutrophil", "CytotoxicT", "Macrophage", "GobletCell", "Fibroblast"):
        ang = SPOKES[br]
        grp = by_branch[br]
        root = [n for n in grp if n["kind"] == "TowerRoot"][0]
        cap = [n for n in grp if n["kind"] == "Capstone"][0]
        stats = [n for n in grp if n["kind"] == "Stat"]
        pos[root["k"]], size[root["k"]] = polar(ang, ROOT_R), 118
        root["axis"], root["r"] = ang, ROOT_R
        last = ROOT_R
        for n, pnt, r in fan(stats, ang, 480):
            pos[n["k"]], size[n["k"]] = pnt, 64
            n["root"], n["axis"], n["r"] = root["k"], ang, r
            last = r
        cap_r = last + 180
        pos[cap["k"]], size[cap["k"]] = polar(ang, cap_r), 100
        cap["root"], cap["axis"], cap["r"] = root["k"], ang, cap_r
        spoke_end[br] = (root["k"], cap_r)
    return (WC, WC), pos, size, spoke_end


def curve(p0, p1, bend=0.0):
    mx, my = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    return f"M{p0[0]:.0f} {p0[1]:.0f} Q{mx - dy * bend:.0f} {my + dx * bend:.0f} {p1[0]:.0f} {p1[1]:.0f}"


def vein(d, vk, outer=16, inner=9, flow=False):
    f = (f'<path class="anim-flow" d="{d}" stroke="#ffffff" stroke-opacity="0.35" stroke-width="{max(2, inner // 3)}" fill="none" stroke-dasharray="4 18" stroke-linecap="round"/>' if flow else "")
    return (f'<path d="{d}" stroke="{PLUM}" stroke-width="{outer}" fill="none" stroke-linecap="round"/>'
            f'<path d="{d}" stroke="{{{{v.{vk}.vein}}}}" stroke-width="{inner}" fill="none" stroke-linecap="round"/>{f}')


def node_button(n, x, y, S):
    k = n["k"]
    kind = n["kind"]
    seed = sum(map(ord, k))
    if kind == "Capstone":
        pts = []
        for i in range(16):
            a = i / 16 * 2 * math.pi
            r = 44 if i % 2 == 0 else 34
            pts.append((r * math.cos(a), r * math.sin(a)))
        shape = smooth_closed(pts)
        inner_shape = smooth_closed([(px * 0.72, py * 0.72) for px, py in pts])
        extra = f'<path d="{shape}" fill="none" stroke="{GOLD}" stroke-width="5" transform="scale(1.12)"/>'
    else:
        pts = blob_pts(0, 0, 40, 39, seed, 1.8, 2.0, 24, (3, 5))
        shape = smooth_closed(pts)
        inner_shape = smooth_closed([(px * 0.84, py * 0.84) for px, py in pts])
        extra = ""
    gs = 0.95 if kind in ("TowerRoot",) else (0.9 if kind == "AbilityRoot" else 0.95)
    pips = ""
    if n["max"] > 1:
        m = n["max"]
        for i in range(m):
            a = math.radians(90 + (i - (m - 1) / 2) * 23)
            pips += f'<circle cx="{47 * math.cos(a):.1f}" cy="{47 * math.sin(a):.1f}" r="8.5" fill="{{{{v.{k}.p{i}}}}}" stroke="{PLUM}" stroke-width="3.5"/>'
    svg = (f'<svg width="{S}" height="{S}" viewBox="-50 -50 100 100" aria-hidden="true" style="overflow: visible">'
           f'<g opacity="{{{{v.{k}.halo}}}}"><circle class="anim-halo" r="56" fill="#fff" fill-opacity="0.22" stroke="#fff" stroke-width="4"/></g>'
           f'<g opacity="{{{{v.{k}.sel}}}}"><circle class="anim-spin" r="62" fill="none" stroke="#fff" stroke-width="6" stroke-dasharray="14 10" stroke-linecap="round"/></g>'
           f'<g opacity="{{{{v.{k}.op}}}}">'
           f'<path d="{shape}" fill="#1E0010" fill-opacity="0.35" transform="translate(0 6)"/>{extra}'
           f'<path d="{shape}" fill="{{{{v.{k}.fill}}}}" stroke="{PLUM}" stroke-width="5"/>'
           f'<path d="{inner_shape}" fill="none" stroke="{{{{v.{k}.rim}}}}" stroke-width="5"/>'
           f'{glyph_svg(n["glyph"], gs)}{pips}</g></svg>')
    return (f'<button onClick="{{{{h.{k}}}}}" aria-label="{n["name"]}" style="position: absolute; left: {x - S / 2:.0f}px; top: {y - S / 2:.0f}px; width: {S}px; height: {S}px; '
            f'padding: 0; border: none; background: transparent; cursor: pointer">{svg}</button>')


def lymph_node_hub(x, y):
    body = smooth_closed([(-96, -30), (-70, -78), (-10, -92), (54, -84), (96, -40), (100, 20), (70, 70), (20, 88), (-20, 70), (-34, 34), (-70, 60), (-100, 30)])
    fol = "".join(f'<circle cx="{fx}" cy="{fy}" r="{fr}" fill="{BODY_HI}" stroke="{LIL_DEEP}" stroke-width="3"/><circle cx="{fx}" cy="{fy}" r="{fr * 0.45:.0f}" fill="{NUC}" fill-opacity="0.8"/>'
                  for fx, fy, fr in [(-60, -34, 18), (-18, -58, 17), (30, -56, 18), (66, -22, 16), (68, 26, 15), (30, 52, 14), (-66, 12, 14)])
    return (f'<g transform="translate({x} {y})"><ellipse cx="0" cy="96" rx="96" ry="14" fill="#1E0010" fill-opacity="0.4"/>'
            f'<path d="{body}" fill="{BODY}" stroke="{PLUM}" stroke-width="7"/>'
            f'<path d="{body}" fill="none" stroke="{LIL_FILL}" stroke-width="3" transform="scale(0.9)"/>{fol}'
            f'</g>')


INIT_LV = {"neutrophil_unlock": 1, "neutrophil_round_damage": 3, "neutrophil_volley_cadence": 2, "neutrophil_aggro_range": 1, "neutrophil_tower_health": 2,
           "cytotoxic_unlock": 1, "cytotoxic_drain": 1, "hub_bone_marrow_reserve": 2, "hub_rapid_metabolism": 1, "hub_cellular_resilience": 1,
           "ability_histamine_unlock": 1, "ability_histamine_cooldown": 1}

TREE_JS = r"""
var N = __NODES__;
var INIT = __INIT__;
var BR = __BR__;
var GLYPHS = __GLYPHS__;
var THRESH = 6;
var WC = __WC__, MINI = __MINI__, VCX = 960, VCY = 640, Z0 = 0.8;
var BY = {};
N.forEach(function (n) { BY[n.k] = n; });
function pts(lv, branch) {
  var s = 0;
  N.forEach(function (n) { if (n.branch === branch && n.kind === 'Stat') s += (lv[n.k] || 0); });
  return s;
}
function cost(n, cur) {
  if (n.kind === 'TowerRoot' || n.kind === 'AbilityRoot') return { m: 0, a: 1 };
  if (n.kind === 'Capstone') return { m: 150, a: 1 };
  var base = 20 + 15 * cur;
  return { m: Math.floor((base * n.w + 50) / 100), a: 0 };
}
function prereq(n, lv) {
  if (n.kind === 'Economy' || n.kind === 'TowerRoot' || n.kind === 'AbilityRoot') return true;
  if (n.kind === 'Capstone') return (lv[n.root] || 0) > 0 && pts(lv, n.branch) >= THRESH;
  return (lv[n.root] || 0) > 0;
}
class Component extends DCLogic {
  renderVals() {
    var self = this;
    if (!this.cam) this.cam = { x: 0, y: 0, z: Z0 };
    var apply = function () {
      var c = self.cam, tx = VCX - WC * c.z + c.x, ty = VCY - WC * c.z + c.y;
      if (self.worldEl) self.worldEl.style.transform = 'translate(' + tx + 'px, ' + ty + 'px) scale(' + c.z + ')';
      if (self.miniEl) {
        var m = MINI / (2 * WC);
        self.miniEl.style.left = (-tx / c.z * m) + 'px';
        self.miniEl.style.top = (-ty / c.z * m) + 'px';
        self.miniEl.style.width = (1920 / c.z * m) + 'px';
        self.miniEl.style.height = (1080 / c.z * m) + 'px';
      }
    };
    var zoomAt = function (sx, sy, f) {
      var c = self.cam;
      var z2 = Math.max(0.3, Math.min(1.6, c.z * f));
      var tx = VCX - WC * c.z + c.x, ty = VCY - WC * c.z + c.y;
      var wx = (sx - tx) / c.z, wy = (sy - ty) / c.z;
      c.x = (sx - wx * z2) - (VCX - WC * z2);
      c.y = (sy - wy * z2) - (VCY - WC * z2);
      c.z = z2;
      apply();
    };
    var frame = function (e) {
      var r = e.currentTarget.getBoundingClientRect();
      return { s: 1920 / r.width, l: r.left, t: r.top };
    };
    var st = this.state || {};
    var lv = st.lv || INIT;
    var mem = st.mem != null ? st.mem : 486;
    var ab = st.ab != null ? st.ab : 1;
    var sel = st.sel || 'neutrophil_capstone';
    var v = {}, h = {}, bp = {};
    ['Neutrophil', 'CytotoxicT', 'Macrophage', 'GobletCell', 'Fibroblast'].forEach(function (b) {
      bp[b] = Math.min(pts(lv, b), THRESH) + '/' + THRESH;
    });
    N.forEach(function (n) {
      var cur = lv[n.k] || 0, ok = prereq(n, lv), maxed = cur >= n.max, c = cost(n, cur);
      var afford = mem >= c.m && ab >= c.a;
      var status = maxed ? 'maxed' : (!ok ? 'locked' : (afford ? 'avail' : 'short'));
      var o = {};
      o.fill = maxed ? '#A58DF2' : (cur > 0 ? '#DCD2FD' : (status === 'locked' ? '#D4CCD9' : '#F7F3FF'));
      o.rim = cur > 0 ? '#6A4FD0' : (status === 'avail' ? '#A48CF0' : (status === 'short' ? '#BDB3CE' : '#A79DB2'));
      o.op = status === 'locked' ? 0.5 : 1;
      o.halo = status === 'avail' ? 1 : 0;
      o.sel = n.k === sel ? 1 : 0;
      o.vein = cur > 0 ? '#A48CF0' : '#CFC7D6';
      for (var i = 0; i < n.max; i++) o['p' + i] = i < cur ? '#48297F' : '#FFFFFF';
      v[n.k] = o;
      h[n.k] = function () { if (self.moved) return; self.setState({ sel: n.k }); };
    });
    var n = BY[sel], cur = lv[n.k] || 0, c = cost(n, cur), ok = prereq(n, lv), maxed = cur >= n.max;
    var afford = mem >= c.m && ab >= c.a;
    var canBuy = !maxed && ok && afford;
    var req = '';
    if (!ok && n.kind === 'Capstone') req = 'Needs ' + THRESH + ' points in ' + BR[n.branch];
    else if (!ok) req = 'Needs ' + BY[n.root].name;
    var show = {};
    GLYPHS.forEach(function (g) { show[g] = g === n.glyph ? 'block' : 'none'; });
    var d = {
      name: n.name, desc: n.desc, req: req, reqShow: req ? 'block' : 'none',
      level: n.max > 1 ? (cur + '/' + n.max) : '',
      btn: maxed ? 'Maxed' : (!ok ? 'Locked' : 'Grow'),
      btnBg: canBuy ? '#6A4FD0' : '#D4CCD9', btnFg: canBuy ? '#FFFFFF' : '#6B5E78',
      mem: c.m, ab: c.a,
      showMem: (c.m > 0 && !maxed) ? 'flex' : 'none', showAb: (c.a > 0 && !maxed) ? 'flex' : 'none',
      show: show
    };
    return {
      v: v, h: h, bp: bp, d: d, mem: mem, ab: ab,
      grow: function () {
        if (!canBuy) return;
        var nl = Object.assign({}, lv);
        nl[n.k] = cur + 1;
        self.setState({ lv: nl, mem: mem - c.m, ab: ab - c.a });
      },
      setWorld: function (el) { self.worldEl = el; apply(); },
      setMini: function (el) { self.miniEl = el; apply(); },
      down: function (e) {
        var f = frame(e);
        self.drag = { sx: e.clientX, sy: e.clientY, x: self.cam.x, y: self.cam.y, s: f.s };
        self.moved = false;
      },
      move: function (e) {
        var g = self.drag;
        if (!g) return;
        var dx = (e.clientX - g.sx) * g.s, dy = (e.clientY - g.sy) * g.s;
        if (Math.abs(dx) + Math.abs(dy) > 6) self.moved = true;
        self.cam.x = g.x + dx;
        self.cam.y = g.y + dy;
        apply();
      },
      up: function () { self.drag = null; },
      wheel: function (e) {
        var f = frame(e);
        zoomAt((e.clientX - f.l) * f.s, (e.clientY - f.t) * f.s, e.deltaY < 0 ? 1.15 : 1 / 1.15);
      },
      zoomIn: function () { zoomAt(VCX, VCY, 1.25); },
      zoomOut: function () { zoomAt(VCX, VCY, 0.8); },
      recenter: function () { self.cam = { x: 0, y: 0, z: Z0 }; apply(); }
    };
  }
}
"""


def scene_tree():
    nodes = parse_tree()
    for n in nodes:
        n["glyph"] = glyph_of(n)
    (HX, HY), pos, size, spoke_end = tree_layout(nodes)
    silk = ""
    angs = sorted(SPOKES.values())
    for r in ():  # no literal web strands; the radial shape is the web
        for a0, a1 in zip(angs, angs[1:] + [angs[0] + 360]):
            p0, p1 = polar(a0, r), polar(a1, r)
            c = polar((a0 + a1) / 2, r * 0.9)
            silk += f'<path d="M{p0[0]:.0f} {p0[1]:.0f} Q{c[0]:.0f} {c[1]:.0f} {p1[0]:.0f} {p1[1]:.0f}" stroke="#FFE3EC" stroke-opacity="0.22" stroke-width="4" fill="none"/>'
    veins = ""
    for br, ang in SPOKES.items():
        if br == "Core":
            e = polar(ang, spoke_end["Core"])
            d = f"M{HX} {HY} L{e[0]:.0f} {e[1]:.0f}"
            veins += (f'<path d="{d}" stroke="{PLUM}" stroke-width="24" fill="none" stroke-linecap="round"/>'
                      f'<path d="{d}" stroke="{LIL_RIM}" stroke-width="13" fill="none" stroke-linecap="round"/>')
            continue
        rk, end_r = spoke_end[br]
        e = polar(ang, end_r)
        veins += vein(f"M{HX} {HY} L{e[0]:.0f} {e[1]:.0f}", rk, 26 if end_r > 700 else 20, 14 if end_r > 700 else 10, flow=True)
    for n in nodes:
        k = n["k"]
        if n["kind"] in ("Stat", "AbilityStat", "Economy"):
            x, y = pos[k]
            a = polar(n["axis"], n["r"] - 70)
            veins += vein(curve(a, (x, y), 0.12), k, 12, 6)
    world_svg = (f'<svg width="{WS}" height="{WS}" viewBox="0 0 {WS} {WS}" style="position: absolute; left: 0; top: 0; overflow: visible" aria-hidden="true">'
                 f'{silk}{veins}<g transform="translate({HX} {HY}) scale(1.05) translate({-HX} {-HY})">{lymph_node_hub(HX, HY)}</g></svg>')
    buttons = "".join(node_button(n, *pos[n["k"]], size[n["k"]]) for n in nodes)
    labels = ""
    for n in nodes:
        if n["kind"] in ("TowerRoot", "AbilityRoot", "Capstone"):
            lx, ly = polar(n["axis"], n["r"] + size[n["k"]] / 2 + 34)
            if n["kind"] == "Capstone":
                text, bg, fg, ex = n["name"] + " · {{bp." + n["branch"] + "}}", GOLD, INK, f"border: 3px solid {PLUM}"
            else:
                text = {"Complement Cascade Burst": "Complement Cascade"}.get(n["name"], n["name"])
                bg, fg, ex = PLUM, "#fff", ""
            labels += (f'<div style="position: absolute; left: {lx:.0f}px; top: {ly:.0f}px; transform: translate(-50%, -50%); pointer-events: none">'
                       f'{pill(text, bg, fg, 22, ex + "; padding: 6px 16px")}</div>')
    tx0, ty0 = 960 - WC * 0.8, 640 - WC * 0.8
    world = (f'<div ref="{{{{setWorld}}}}" style="position: absolute; left: 0; top: 0; width: {WS}px; height: {WS}px; transform-origin: 0 0; '
             f'transform: translate({tx0:.0f}px, {ty0:.0f}px) scale(0.8)">{world_svg}{buttons}{labels}</div>')
    viewport = (f'<div onMouseDown="{{{{down}}}}" onMouseMove="{{{{move}}}}" onMouseUp="{{{{up}}}}" onMouseLeave="{{{{up}}}}" onWheel="{{{{wheel}}}}" '
                f'style="position: absolute; inset: 0; overflow: hidden; cursor: grab; user-select: none">{world}</div>')
    # ---- minimap: the whole web, the visible window drawn over it
    m = MINI / WS
    mini_svg = ""
    for br, ang in SPOKES.items():
        end_r = spoke_end["Core"] if br == "Core" else spoke_end[br][1]
        e = polar(ang, end_r)
        mini_svg += f'<path d="M{HX * m:.1f} {HY * m:.1f} L{e[0] * m:.1f} {e[1] * m:.1f}" stroke="{LIL_RIM}" stroke-width="2.5"/>'
    for n in nodes:
        x, y = pos[n["k"]]
        rr = 5 if n["kind"] in ("TowerRoot", "Capstone", "AbilityRoot") else 3
        mini_svg += f'<circle cx="{x * m:.1f}" cy="{y * m:.1f}" r="{rr}" fill="{{{{v.{n["k"]}.fill}}}}" stroke="{PLUM}" stroke-width="1.2"/>'
    mw0 = 1920 / 0.8 * m
    mh0 = 1080 / 0.8 * m
    minimap = (f'<div style="position: absolute; left: {W - 30 - MINI}px; top: {H - 110 - MINI}px; width: {MINI}px; height: {MINI}px; border-radius: 50%; '
               f'background: rgba(30,0,16,0.72); border: 4px solid {PLUM}; box-shadow: 0 0 0 3px {LIL_RIM}; overflow: hidden; pointer-events: none">'
               f'<svg width="{MINI}" height="{MINI}" viewBox="0 0 {MINI} {MINI}" style="position: absolute; left: 0; top: 0" aria-hidden="true">{mini_svg}</svg>'
               f'<div ref="{{{{setMini}}}}" style="position: absolute; left: {-tx0 / 0.8 * m:.1f}px; top: {-ty0 / 0.8 * m:.1f}px; width: {mw0:.1f}px; height: {mh0:.1f}px; '
               f'border: 2.5px solid #fff; border-radius: 6px; box-sizing: border-box"></div></div>')

    def round_btn(handler, icon, aria, x):
        blob = smooth_closed(blob_pts(32, 32, 27, 26, len(aria), 1.6, 2.2, 28))
        return (f'<button onClick="{{{{{handler}}}}}" aria-label="{aria}" style="position: absolute; left: {x}px; top: {H - 94}px; width: 68px; height: 68px; padding: 0; '
                f'border: none; background: transparent; cursor: pointer"><svg width="68" height="68" viewBox="0 0 64 64" aria-hidden="true">'
                f'<path d="{blob}" fill="{LIL_FILL}" stroke="{PLUM}" stroke-width="3.5"/>{icon}</svg></button>')
    zx = W - 30 - MINI + 6
    controls = (round_btn("zoomOut", f'<path d="M21 32h22" stroke="{PLUM}" stroke-width="5.5" stroke-linecap="round"/>', "Zoom out", zx)
                + round_btn("zoomIn", f'<path d="M21 32h22M32 21v22" stroke="{PLUM}" stroke-width="5.5" stroke-linecap="round"/>', "Zoom in", zx + 80)
                + round_btn("recenter", f'<circle cx="32" cy="32" r="11" fill="none" stroke="{PLUM}" stroke-width="4.5"/><circle cx="32" cy="32" r="3.5" fill="{PLUM}"/>'
                            f'<path d="M32 14v6M32 44v6M14 32h6M44 32h6" stroke="{PLUM}" stroke-width="4.5" stroke-linecap="round"/>', "Recenter", zx + 160))
    back = (f'<a href="Menu.dc.html" aria-label="Back to main menu" style="position: absolute; left: 36px; top: 32px; width: 56px; height: 56px; display: block; cursor: pointer">'
            f'<svg width="56" height="56" viewBox="0 0 64 64" aria-hidden="true"><path d="{smooth_closed(blob_pts(32, 32, 26, 25, 5, 1.8, 2.2, 28))}" fill="{LIL_FILL}" stroke="{PLUM}" stroke-width="3.5"/>'
            f'<path d="M36 20 24 32l12 12" stroke="{PLUM}" stroke-width="5" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg></a>')
    title = (f'<div style="position: absolute; left: 110px; top: 30px; font-family: {DISPLAY}; font-weight: 700; font-size: 44px; line-height: 1.05; color: {LIL_FILL}; '
             f'-webkit-text-stroke: 5px {PLUM}; paint-order: stroke fill; pointer-events: none">Strengthen Immunity</div>')
    wallet = (f'<div style="position: absolute; left: 1440px; top: 32px; display: flex; gap: 10px">'
              f'<div style="display: flex; align-items: center; gap: 6px; height: 52px; padding: 0 16px 0 8px; box-sizing: border-box; border-radius: 999px; background: {LIL_FILL}; border: 3px solid {PLUM}">'
              f'{memory_icon(32)}<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 24px">{{{{mem}}}}</span></div>'
              f'<div style="display: flex; align-items: center; gap: 6px; height: 52px; padding: 0 16px 0 8px; box-sizing: border-box; border-radius: 999px; background: #FFF2D2; border: 3px solid {PLUM}">'
              f'{antibody_icon(32)}<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 24px">{{{{ab}}}}</span></div></div>')
    play = link_cell("Levels.dc.html", 1690, 18, 200, 80,
                     f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 34px; color: #fff; text-shadow: 0 3px 0 {PLUM}">Play</span>'
                     f'<svg width="28" height="28" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4.5v15L19.5 12Z" fill="#fff" stroke="{PLUM}" stroke-width="2" stroke-linejoin="round"/></svg>',
                     LIL_DEEP, LIL_RIM, 21, aria="Play", rimw=7)
    glyph_keys = sorted({n["glyph"] for n in nodes})
    icons = "".join(f'<svg style="display: {{{{d.show.{g}}}}}; overflow: visible" width="54" height="54" viewBox="-60 -60 120 120" aria-hidden="true">{glyph_svg(g, 1.2)}</svg>' for g in glyph_keys)
    strip = (f'<div style="display: flex; align-items: center; gap: 14px; height: 100%">'
             f'<div style="width: 64px; height: 64px; border-radius: 50%; background: #fff; border: 3px solid {LIL_RIM}; display: flex; align-items: center; justify-content: center; flex-shrink: 0">{icons}</div>'
             f'<div style="flex-grow: 1; display: flex; flex-direction: column; gap: 2px; min-width: 0">'
             f'<div style="display: flex; align-items: baseline; gap: 8px"><span style="font-family: {DISPLAY}; font-weight: 600; font-size: 22px; color: {INK}; white-space: nowrap">{{{{d.name}}}}</span>'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 17px; color: {LIL_DEEP}">{{{{d.level}}}}</span></div>'
             f'<span style="font-family: {TEXTF}; font-size: 15px; font-weight: 700; color: {INK2}; white-space: nowrap; overflow: hidden; text-overflow: ellipsis">{{{{d.desc}}}}</span>'
             f'<span style="display: {{{{d.reqShow}}}}; font-family: {TEXTF}; font-size: 13px; font-weight: 900; color: {RED}">{{{{d.req}}}}</span></div>'
             f'<span style="display: {{{{d.showMem}}}}; align-items: center; gap: 4px; flex-shrink: 0">{memory_icon(24)}<b style="font-family: {DISPLAY}; font-weight: 600; font-size: 21px">{{{{d.mem}}}}</b></span>'
             f'<span style="display: {{{{d.showAb}}}}; align-items: center; gap: 4px; flex-shrink: 0">{antibody_icon(24)}<b style="font-family: {DISPLAY}; font-weight: 600; font-size: 21px">{{{{d.ab}}}}</b></span>'
             f'<button onClick="{{{{grow}}}}" style="height: 54px; min-width: 112px; padding: 0 18px; border-radius: 20px; border: 3px solid {PLUM}; background: {{{{d.btnBg}}}}; '
             f'color: {{{{d.btnFg}}}}; cursor: pointer; font-family: {DISPLAY}; font-weight: 600; font-size: 22px; flex-shrink: 0">{{{{d.btn}}}}</button></div>')
    info = cell_panel(640, 14, 780, 96, strip, "immune", seed=19, pad="0 22px 0 18px", n_exp=4, rimw=5)
    body = backdrop("", 0.5) + viewport + minimap + controls + back + title + info + wallet + play
    js = (TREE_JS.replace("__NODES__", json.dumps([{kk: n[kk] for kk in ("k", "name", "desc", "kind", "branch", "max", "w", "glyph", "root") if kk in n} for n in nodes]))
          .replace("__INIT__", json.dumps(INIT_LV)).replace("__BR__", json.dumps(BR_NAME)).replace("__GLYPHS__", json.dumps(glyph_keys))
          .replace("__WC__", str(WC)).replace("__MINI__", str(MINI)))
    print("tree nodes", len(nodes))
    return page_js("Strengthen Immunity", body, js.strip())


# ================================================================== LEVEL SELECT
REGION = {"capillary": "Capillary", "lymphatic": "Lymphatic", "organ_chamber": "Organ chamber", "mucosal": "Mucosa", "floodplain": "Floodplain", "skin": "Skin breach"}


def load_levels():
    out = []
    for i in range(1, 11):
        f = glob.glob(os.path.join(REPO, "assets", "levels", f"campaign_{i:02d}_*.json"))[0]
        d = json.load(open(f, encoding="utf-8"))
        out.append(d)
    return out


def level_thumb(d, w, h, clip_id=None, round_=False):
    xs, ys = [d["world"]["min"][0], d["world"]["max"][0]], [d["world"]["min"][1], d["world"]["max"][1]]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    pad = 0.04
    sx = w / ((x1 - x0) * (1 + 2 * pad))
    sy = h / ((y1 - y0) * (1 + 2 * pad))
    s = min(sx, sy)
    ox = (w - (x1 - x0) * s) / 2
    oy = (h - (y1 - y0) * s) / 2

    def P(p):
        return ox + (p[0] - x0) * s, h - (oy + (p[1] - y0) * s)

    layers = [[], [], [], []]
    for v in d["vessels"]:
        pts = [P(q["p"]) for q in v["points"]]
        dd = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts)
        wv = max(q["w"] for q in v["points"]) * s
        layers[0].append(f'<path d="{dd}" stroke="{LANE_RIM}" stroke-width="{wv + 5:.1f}" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
        layers[1].append(f'<path d="{dd}" stroke="{LANE_WALL}" stroke-width="{wv + 1.5:.1f}" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
        layers[2].append(f'<path d="{dd}" stroke="{LANE_IN}" stroke-width="{max(1.0, wv - 3):.1f}" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
    for ob in d.get("obstacles", []):
        infl = ob.get("inflate", 0) * s
        if "points" in ob:
            pts = [P(q) for q in ob["points"]]
            if len(pts) >= 3:
                dd = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts) + "Z"
            else:
                dd = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts)
            layers[3].append(f'<path d="{dd}" fill="{CELL}" stroke="{LANE_RIM}" stroke-width="{infl * 2 + 4:.1f}" stroke-linejoin="round" stroke-linecap="round"/>'
                             f'<path d="{dd}" fill="{CELL}" stroke="{CELL}" stroke-width="{max(0.5, infl * 2 - 2):.1f}" stroke-linejoin="round" stroke-linecap="round"/>')
        elif "center" in ob:
            cx, cy = P(ob["center"])
            r = ob.get("radius", 10) * s + infl
            layers[3].append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r + 2:.1f}" fill="{LANE_RIM}"/><circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{CELL}"/>')
    marks = ""
    for sp in d["spawn_points"]:
        x, y = P(sp["pos"])
        x, y = min(max(x, 6), w - 6), min(max(y, 6), h - 6)
        marks += f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{max(4, w / 40):.1f}" fill="#FFE39A" stroke="{PLUM}" stroke-width="2"/>'
    bg = f'<rect width="{w}" height="{h}" fill="{MATRIX}"/>'
    return bg + "".join(layers[0] + layers[1] + layers[2] + layers[3]) + marks


LEVEL_STATUS = ["cleared", "cleared", "cleared", "open"] + ["locked"] * 6
STATION = [(330, 800), (750, 800), (1170, 800), (1590, 800), (1590, 510), (1170, 510), (750, 510), (750, 220), (1170, 220), (1590, 220)]

LEVELS_JS = r"""
var L = __LEVELS__;
class Component extends DCLogic {
  renderVals() {
    var st = this.state || {};
    var sel = st.sel || 0;
    var self = this;
    var v = {}, h = {};
    L.forEach(function (l) {
      v['l' + l.i] = { sel: l.i === sel ? 1 : 0 };
      h['l' + l.i] = function () { if (l.status !== 'locked') self.setState({ sel: l.i }); };
    });
    return { v: v, h: h, playShow: sel ? 'block' : 'none' };
  }
}
"""


def station(i, d, x, y, status):
    S = 156
    cid = f"st{i}"
    rim = {"cleared": LIL_RIM, "open": GOLD, "locked": "#A79DB2"}[status]
    circ = smooth_closed(blob_pts(0, 0, 58, 57, i * 7, 1.6, 2.0, 28))
    thumb = level_thumb(d, 116, 116)
    lock = ""
    if status == "locked":
        lock = (f'<path d="{circ}" fill="#3B2140" fill-opacity="0.55"/>'
                f'<g transform="translate(-14 -18)"><path d="M6 14v-5a8 8 0 0 1 16 0v5" stroke="#fff" stroke-width="4.5" fill="none"/>'
                f'<rect x="1" y="13" width="26" height="22" rx="6" fill="#fff" stroke="{PLUM}" stroke-width="3"/><circle cx="14" cy="24" r="3" fill="{PLUM}"/></g>')
    check = ""
    if status == "cleared":
        check = (f'<circle cx="44" cy="42" r="17" fill="{LIL_DEEP}" stroke="{PLUM}" stroke-width="3.5"/>'
                 f'<path d="m36 42 6 6 10-11" stroke="#fff" stroke-width="4.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
    new = ""
    if status == "open":
        new = f'<circle class="anim-halo" r="72" fill="none" stroke="{GOLD}" stroke-width="6"/>'
    svg = (f'<svg width="{S}" height="{S}" viewBox="-68 -68 136 136" aria-hidden="true" style="overflow: visible">'
           f'<defs><clipPath id="{cid}"><path d="{circ}"/></clipPath></defs>{new}'
           f'<g opacity="{{{{v.l{i}.sel}}}}"><circle class="anim-spin" r="78" fill="none" stroke="#fff" stroke-width="6" stroke-dasharray="16 11" stroke-linecap="round"/></g>'
           f'<path d="{circ}" fill="#1E0010" fill-opacity="0.4" transform="translate(0 7)"/>'
           f'<g clip-path="url(#{cid})"><g transform="translate(-58 -58)">{thumb}</g></g>{lock}'
           f'<path d="{circ}" fill="none" stroke="{rim}" stroke-width="9"/><path d="{circ}" fill="none" stroke="{PLUM}" stroke-width="4" transform="scale(1.09)"/>'
           f'<circle cx="-46" cy="-44" r="19" fill="{NUC if status != "locked" else "#8C8599"}" stroke="{PLUM}" stroke-width="3.5"/>'
           f'<text x="-46" y="-37" text-anchor="middle" font-family="Fredoka, sans-serif" font-weight="600" font-size="21" fill="#fff">{i}</text>{check}</svg>')
    tag = pill(d["display_name"], PLUM if status != "locked" else "#5B4A60", "#fff", 15)
    newtag = ""
    return (f'<button onClick="{{{{h.l{i}}}}}" aria-label="Level {i}: {d["display_name"]}" style="position: absolute; left: {x - S / 2}px; top: {y - S / 2}px; width: {S}px; height: {S + 36}px; '
            f'padding: 0; border: none; background: transparent; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 6px">'
            f'<span style="position: relative; display: block">{svg}{newtag}</span>{tag}</button>')


def scene_levels():
    levels = load_levels()
    route = "M-40 800 L1590 800 C1850 800 1850 510 1590 510 L750 510 C490 510 490 220 750 220 L1960 220"
    done = "M-40 800 L1590 800"
    rsvg = (f'<path d="{route}" stroke="{LANE_RIM}" stroke-width="64" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="{route}" stroke="#D9C6CF" stroke-width="50" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="{route}" stroke="#EBDDE3" stroke-width="26" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="{done}" stroke="{LANE_WALL}" stroke-width="50" fill="none" stroke-linecap="round"/>'
            f'<path d="{done}" stroke="{LANE_IN}" stroke-width="36" fill="none" stroke-linecap="round"/>'
            f'<path class="anim-flow" d="{done}" stroke="{LIL_RIM}" stroke-width="8" fill="none" stroke-dasharray="6 16" stroke-linecap="round"/>')
    stations = "".join(station(i + 1, d, *STATION[i], LEVEL_STATUS[i]) for i, d in enumerate(levels))
    head = (f'<a href="Tree.dc.html" aria-label="Back to Strengthen Immunity" style="position: absolute; left: 36px; top: 32px; width: 56px; height: 56px; display: block; cursor: pointer">'
            f'<svg width="56" height="56" viewBox="0 0 64 64" aria-hidden="true"><path d="{smooth_closed(blob_pts(32, 32, 26, 25, 5, 1.8, 2.2, 28))}" fill="{LIL_FILL}" stroke="{PLUM}" stroke-width="3.5"/>'
            f'<path d="M36 20 24 32l12 12" stroke="{PLUM}" stroke-width="5" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg></a>'
            f'<div style="position: absolute; left: 110px; top: 30px; font-family: {DISPLAY}; font-weight: 700; font-size: 44px; line-height: 1.05; color: {FL_FILL}; '
            f'-webkit-text-stroke: 5px {PLUM}; paint-order: stroke fill">Campaign</div>')
    play = (f'<div style="display: {{{{playShow}}}}">'
            + link_cell("Main.dc.html", 1560, 950, 320, 100,
                        f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 42px; color: #fff; text-shadow: 0 3px 0 {PLUM}">Play</span>'
                        f'<svg width="34" height="34" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4.5v15L19.5 12Z" fill="#fff" stroke="{PLUM}" stroke-width="2" stroke-linejoin="round"/></svg>',
                        LIL_DEEP, LIL_RIM, 23, aria="Play the selected level", rimw=8, cls="anim-wobble")
            + '</div>')
    js = LEVELS_JS.replace("__LEVELS__", json.dumps([{"i": i + 1, "status": LEVEL_STATUS[i]} for i in range(len(levels))]))
    return page_js("IMMUNE level select", backdrop(rsvg, 0.0) + stations + head + play, js.strip())


# ================================================================== RESULTS
def reward_row(icon, label_, value, col=INK):
    return (f'<div style="display: flex; align-items: center; gap: 14px; height: 64px; padding: 0 18px; box-sizing: border-box; border-radius: 20px; background: #fff; border: 2.5px solid {LIL_RIM}">'
            f'{icon}<span style="flex-grow: 1; font-size: 20px; font-weight: 900; color: {INK}">{label_}</span>'
            f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 32px; color: {col}">{value}</span></div>')


def scene_results(win=True):
    if win:
        from hud_screens import TW as _TW
        bg = world("".join(tower_world(k, *p) for k, p in _TW), 72) + dim(0.55)
        title, name = "Level cleared", "First Bend"
        rows = (reward_row(memory_icon(40), "Memory cells", "+203", "#2E5AA8") +
                reward_row(antibody_icon(40), "Antibody", "+1", AMBER_T) +
                reward_row(nucleus_key("2", 36), "Island Climb unlocked", ""))
        btns = [("Tree.dc.html", "Strengthen Immunity", 330, LIL_DEEP, LIL_RIM, "#fff"), ("Levels.dc.html", "Next level", 220, LIL_FILL, LIL_RIM, INK),
                ("Main.dc.html", "Replay", 170, FL_FILL, FL_RIM, INK)]
        kind, fill, rim, hgt = "immune", LIL_FILL, LIL_RIM, 560
    else:
        bg = world(horde(0.55, 0.99, 700, 5, pack=0.8), 0, inflamed=True) + dim(0.6)
        title, name = "Level failed", "Twin Channels"
        rows = reward_row(memory_icon(40), "Memory cells", "+70", "#2E5AA8")
        btns = [("Tree.dc.html", "Strengthen Immunity", 330, LIL_DEEP, LIL_RIM, "#fff"), ("Main.dc.html", "Retry", 170, FL_FILL, FL_RIM, INK),
                ("Levels.dc.html", "Levels", 170, FL_FILL, FL_RIM, INK)]
        kind, fill, rim, hgt = "host", "#FFF0F0", "#E2475F", 420
    bh = "".join(link_cell(href, 0, 0, w, 84, f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 26px; color: {fg}">{t}</span>', f, r, 30 + i,
                           color=fg, aria=t, extra="position: relative; left: auto; top: auto")
                 for i, (href, t, w, f, r, fg) in enumerate(btns))
    inner = (f'<div style="display: flex; flex-direction: column; align-items: center; gap: 14px; height: 100%">'
             f'<span style="font-family: {DISPLAY}; font-weight: 700; font-size: 56px; line-height: 1; color: {INK if win else RED}">{title}</span>'
             f'<span style="font-family: {DISPLAY}; font-weight: 500; font-size: 24px; color: {INK2}">{name}</span>'
             f'<div style="width: 100%; display: flex; flex-direction: column; gap: 12px; margin-top: 8px">{rows}</div>'
             f'<div style="flex-grow: 1"></div><div style="display: flex; gap: 14px; justify-content: center">{bh}</div></div>')
    card = cell_panel((W - 780) // 2, (H - hgt) // 2, 780, hgt, inner, kind, seed=51 if win else 52, pad="40px 44px 36px", fill=fill, rim=rim, rimw=9, n_exp=4)
    return page_js("IMMUNE results, " + ("victory" if win else "defeat"), bg + card, EMPTY_JS)


BOARDS = [("Menu.dc.html", scene_menu), ("Tree.dc.html", scene_tree), ("Levels.dc.html", scene_levels),
          ("Victory.dc.html", lambda: scene_results(True)), ("Defeat.dc.html", lambda: scene_results(False))]
if __name__ == "__main__":
    for name, fn in BOARDS:
        html = fn()
        with open(os.path.join(PROJ, name), "w", encoding="utf-8") as f:
            f.write(html)
        print(name, len(html))
