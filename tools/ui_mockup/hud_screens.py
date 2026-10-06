"""IMMUNE in-match HUD mockup screens (+ the shared visual language menu_screens.py builds on).

Writes one .dc.html per artboard into ./project/ for the Design canvas; see README.md.
"""
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.join(HERE, "project")
os.makedirs(PROJ, exist_ok=True)
W, H = 1920, 1080

# ------------------------------------------------------------------ palette (sampled from the game)
PLUM = "#4E1638"          # the cartoon outline every vessel has
MATRIX = "#992848"        # extracellular matrix
CELL = "#6C1834"          # tissue cell body
CELL_NUC = "#8C2A4B"
CELL_NUC2 = "#A63C5F"
LANE_RIM = "#5E1F48"
LANE_WALL = "#E3808F"
LANE_IN = "#F9C8A8"
LANE_CORE = "#FDDAB8"

# immune (friendly) = lavender cells, like the towers and swarmers in-game
LIL_FILL = "#F2EDFF"
LIL_RIM = "#A48CF0"
LIL_DEEP = "#6A4FD0"
NUC = "#48297F"
BODY = "#A58DF2"
BODY_HI = "#C9BCFB"

# host = flesh
FL_FILL = "#FFF0E6"
FL_RIM = "#EE95A0"
FL_DEEP = "#C24A66"

INK = "#2B1030"
INK2 = "#5E4470"
INK3 = "#7D6590"

ATP_C = "#FF8A5B"          # mitochondrion
ATP_D = "#A8401F"
BACT = "#EDF263"
BACT_D = "#8E8F1A"
VIRUS = "#3CD46C"
VIRUS_D = "#15672F"
PARA = "#8A5226"
PARA_D = "#3B200C"
GOLD = "#F2B233"
GOLD_D = "#8A5A00"
RED = "#D7263D"
AMBER_T = "#9A5A00"        # amber for text on light fills
AMBER = "#F3A42A"

DISPLAY = "'Fredoka', 'Trebuchet MS', sans-serif"
TEXTF = "'Nunito', 'Segoe UI', sans-serif"

_uid = [0]


def uid(p="u"):
    _uid[0] += 1
    return f"{p}{_uid[0]}"


# ------------------------------------------------------------------ organic geometry
def smooth_closed(pts):
    n = len(pts)
    d = f"M{pts[0][0]:.1f} {pts[0][1]:.1f}"
    for i in range(n):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        d += f" C{c1[0]:.1f} {c1[1]:.1f} {c2[0]:.1f} {c2[1]:.1f} {p2[0]:.1f} {p2[1]:.1f}"
    return d + "Z"


def blob_pts(cx, cy, a, b, seed, amp=3.0, n_exp=5.0, count=48, lobes=(3, 5)):
    """Superellipse (rounded-rect-ish) with a gentle membrane wobble."""
    rnd = random.Random(seed)
    ph = [rnd.uniform(0, 6.28) for _ in range(3)]
    k1, k2 = rnd.randint(*lobes), rnd.randint(lobes[1], lobes[1] + 3)
    pts = []
    for i in range(count):
        t = i / count * 2 * math.pi
        c, s = math.cos(t), math.sin(t)
        x = a * math.copysign(abs(c) ** (2 / n_exp), c)
        y = b * math.copysign(abs(s) ** (2 / n_exp), s)
        L = math.hypot(x, y) or 1
        w = amp * (0.65 * math.sin(k1 * t + ph[0]) + 0.35 * math.sin(k2 * t + ph[1]))
        pts.append((cx + x + x / L * w, cy + y + y / L * w))
    return pts


def membrane_svg(w, h, fill, rim, seed, rimw=6, amp=3.0, n_exp=5.0, organelles=True, nucleus=None, outline=PLUM):
    """A panel drawn as a cell: plum outline, coloured membrane band, cytoplasm, a few organelles."""
    cx, cy = w / 2, h / 2
    outer = smooth_closed(blob_pts(cx, cy, w / 2 - 4, h / 2 - 4, seed, amp, n_exp))
    inner = smooth_closed(blob_pts(cx, cy, w / 2 - 4 - rimw, h / 2 - 4 - rimw, seed, amp, n_exp))
    deco = ""
    if organelles:
        rnd = random.Random(seed * 3 + 1)
        for _ in range(max(3, int(w * h / 14000))):
            side = rnd.choice(["t", "b", "l", "r"])
            if side in "tb":
                x = rnd.uniform(0.1, 0.9) * w
                y = (rimw + 12 + rnd.uniform(0, 6)) if side == "t" else (h - rimw - 12 - rnd.uniform(0, 6))
            else:
                y = rnd.uniform(0.2, 0.8) * h
                x = (rimw + 12 + rnd.uniform(0, 6)) if side == "l" else (w - rimw - 12 - rnd.uniform(0, 6))
            r = rnd.uniform(2.2, 4.2)
            deco += f'<ellipse cx="{x:.0f}" cy="{y:.0f}" rx="{r * 1.4:.1f}" ry="{r:.1f}" fill="{rim}" fill-opacity="0.28"/>'
    nuc = ""
    if nucleus:
        nx, ny, nr, ncol = nucleus
        nuc = (f'<path d="{smooth_closed(blob_pts(nx, ny, nr, nr * 0.86, seed + 9, 1.6, 2.2, 24))}" fill="{ncol}" fill-opacity="0.16"/>')
    return (f'<svg width="{w}" height="{h}" viewBox="0 0 {w} {h}" style="position: absolute; left: 0; top: 0; overflow: visible" aria-hidden="true">'
            f'<path d="{outer}" fill="rgba(40,0,20,0.28)" transform="translate(0 5)"/>'
            f'<path d="{outer}" fill="{rim}" stroke="{outline}" stroke-width="3.5"/>'
            f'<path d="{inner}" fill="{fill}"/>'
            f'<path d="{inner}" fill="none" stroke="#ffffff" stroke-opacity="0.55" stroke-width="2" stroke-dasharray="0 0"/>'
            f'{nuc}{deco}</svg>')


def cell_panel(x, y, w, h, inner_html, kind="immune", seed=1, pad="16px 20px", extra="", rimw=6, amp=3.0, n_exp=5.0,
               fill=None, rim=None, anchor="left", cls=""):
    f = fill or (LIL_FILL if kind == "immune" else FL_FILL)
    r = rim or (LIL_RIM if kind == "immune" else FL_RIM)
    pos = f"left: {x}px;" if anchor == "left" else f"right: {x}px;"
    c = f' class="{cls}"' if cls else ""
    return (f'<div{c} style="position: absolute; {pos} top: {y}px; width: {w}px; height: {h}px; {extra}">'
            f'{membrane_svg(w, h, f, r, seed, rimw, amp, n_exp)}'
            f'<div style="position: relative; box-sizing: border-box; width: {w}px; height: {h}px; padding: {pad}">{inner_html}</div></div>')


# ------------------------------------------------------------------ small illustrations (flat, outlined)
def svgwrap(inner, size, vb=64, extra=""):
    return (f'<svg width="{size}" height="{size}" viewBox="0 0 {vb} {vb}" fill="none" stroke-linecap="round" stroke-linejoin="round" '
            f'aria-hidden="true" style="flex-shrink: 0; overflow: visible; {extra}">{inner}</svg>')


def mito_inner(cx=32, cy=32, s=1.0, rot=-20):
    return (f'<g transform="translate({cx} {cy}) rotate({rot}) scale({s})">'
            f'<rect x="-26" y="-14" width="52" height="28" rx="14" fill="{ATP_C}" stroke="{ATP_D}" stroke-width="3"/>'
            f'<path d="M-19 0c3-9 6 9 9 0s6 9 9 0 6 9 9 0 6 9 9 0" stroke="{ATP_D}" stroke-width="2.6"/>'
            f'<ellipse cx="-10" cy="-7" rx="7" ry="2.5" fill="#fff" fill-opacity="0.45"/></g>')


def mito(size=24):
    return svgwrap(mito_inner(), size)


def memory_icon(size=22):
    return svgwrap(f'<circle cx="32" cy="32" r="24" fill="#A9D8FF" stroke="#244C8F" stroke-width="3.5"/>'
                   f'<circle cx="34" cy="33" r="15" fill="#2E5AA8"/><ellipse cx="22" cy="20" rx="6" ry="3" fill="#fff" fill-opacity="0.6"/>', size)


def antibody_icon(size=22):
    return svgwrap(f'<path d="M32 58V34M32 34 14 12M32 34l18-22" stroke="{GOLD_D}" stroke-width="12"/>'
                   f'<path d="M32 58V34M32 34 14 12M32 34l18-22" stroke="{GOLD}" stroke-width="7"/>', size)


def bacteria_inner(cx, cy, s=1.0, rot=-25, crown=False):
    c = (f'<g transform="translate(-9 -27)"><path d="M0 12 3 2l6 6 5-8 5 8 6-6 3 10Z" fill="{GOLD}" stroke="{GOLD_D}" stroke-width="2.4"/></g>' if crown else "")
    return (f'<g transform="translate({cx} {cy}) scale({s})"><g transform="rotate({rot})">'
            f'<path d="M22 0c6-3 7 4 12 1s6 3 10 0" stroke="{BACT_D}" stroke-width="2.4"/>'
            f'<path d="M20 -5c5-6 8 0 12-5" stroke="{BACT_D}" stroke-width="2.2"/>'
            f'<rect x="-24" y="-10" width="48" height="20" rx="10" fill="{BACT}" stroke="{BACT_D}" stroke-width="3"/>'
            f'<ellipse cx="-8" cy="-4" rx="9" ry="2.6" fill="#fff" fill-opacity="0.55"/></g>{c}</g>')


def virus_inner(cx, cy, s=1.0, crown=False):
    spikes = ""
    for i in range(10):
        a = i / 10 * 2 * math.pi
        x1, y1 = 15 * math.cos(a), 15 * math.sin(a)
        x2, y2 = 22 * math.cos(a), 22 * math.sin(a)
        spikes += (f'<path d="M{x1:.1f} {y1:.1f}L{x2:.1f} {y2:.1f}" stroke="{VIRUS_D}" stroke-width="3"/>'
                   f'<circle cx="{x2:.1f}" cy="{y2:.1f}" r="3.6" fill="{VIRUS}" stroke="{VIRUS_D}" stroke-width="2"/>')
    c = (f'<g transform="translate(-14 -44) scale(1.5)"><path d="M0 12 3 2l6 6 5-8 5 8 6-6 3 10Z" fill="{GOLD}" stroke="{GOLD_D}" stroke-width="2"/></g>' if crown else "")
    return (f'<g transform="translate({cx} {cy}) scale({s})">{spikes}<circle r="16" fill="{VIRUS}" stroke="{VIRUS_D}" stroke-width="3"/>'
            f'<circle cx="-5" cy="-3" r="4" fill="{VIRUS_D}" fill-opacity="0.35"/><circle cx="5" cy="5" r="3" fill="{VIRUS_D}" fill-opacity="0.35"/>'
            f'<ellipse cx="-6" cy="-9" rx="5" ry="2.2" fill="#fff" fill-opacity="0.55"/>{c}</g>')


def parasite_inner(cx, cy, s=1.0):
    d = "M-24 10c6-20 14 8 22-8s14 6 24-12"
    return (f'<g transform="translate({cx} {cy}) scale({s})"><path d="{d}" stroke="{PARA_D}" stroke-width="15"/>'
            f'<path d="{d}" stroke="{PARA}" stroke-width="10"/><path d="{d}" stroke="#C07A45" stroke-width="2.5" stroke-dasharray="3 6"/>'
            f'<circle cx="22" cy="-10" r="2.4" fill="{PARA_D}"/></g>')


def germ(fam, size=30, crown=False):
    if fam == "bacteria":
        inner = bacteria_inner(30, 34, 1.05, crown=crown)
    elif fam == "virus":
        inner = virus_inner(32, 34, 1.15, crown=crown)
    else:
        inner = parasite_inner(32, 34, 1.05)
    return svgwrap(inner, size)


FAM_NAME = {"bacteria": "Bacteria", "virus": "Viruses", "parasite": "Parasites"}
FAM_COL = {"bacteria": BACT, "virus": VIRUS, "parasite": PARA}
FAM_D = {"bacteria": BACT_D, "virus": VIRUS_D, "parasite": PARA_D}


def organ_inner(cx=32, cy=32, s=1.0, col="#F08A9C", deep=FL_DEEP):
    return (f'<g transform="translate({cx} {cy}) scale({s})">'
            f'<path d="M-4 -22c0-8 4-12 8-12M6 -20c2-8 8-10 12-9" stroke="{PLUM}" stroke-width="8"/>'
            f'<path d="M-4 -22c0-8 4-12 8-12M6 -20c2-8 8-10 12-9" stroke="#C85C7C" stroke-width="4.5"/>'
            f'<path d="M0 26C-18 18-28 6-27-8c1-10 9-16 17-15 5 0 8 3 10 6 2-3 6-6 11-6 8 0 16 7 16 16C27 7 18 18 0 26Z" fill="{col}" stroke="{PLUM}" stroke-width="3.5"/>'
            f'<path d="M-2 -10c-2 10 2 20 10 26" stroke="{deep}" stroke-width="2.6" stroke-opacity="0.6"/>'
            f'<ellipse cx="-14" cy="-6" rx="6" ry="3.5" fill="#fff" fill-opacity="0.5"/></g>')


# ------------------------------------------------------------------ tower illustrations (64 space)
TOWERS = [
    ("neutrophil", "Neutrophil", "Squad shooter", 70, "Attack"),
    ("cytotoxic_t", "Cytotoxic T", "Latch &amp; drain", 130, "Attack"),
    ("macrophage", "Macrophage", "Grab &amp; engulf", 180, "Attack"),
    ("goblet_cell", "Goblet Cell", "Mucus slows", 160, "Control"),
    ("fibroblast", "Fibroblast", "Lays scar walls", 120, "Control"),
]
TOWER = {t[0]: t for t in TOWERS}


def tower_inner(key, grey=False):
    body, hi, rim, nuc = (BODY, BODY_HI, LIL_DEEP, NUC) if not grey else ("#C9C3D6", "#E2DEEA", "#8C8599", "#6C6478")
    if key == "neutrophil":
        b = smooth_closed(blob_pts(32, 33, 23, 22, 11, 2.4, 2.0, 30, (5, 7)))
        return (f'<path d="{b}" fill="{body}" stroke="{rim}" stroke-width="3"/>'
                f'<path d="{smooth_closed(blob_pts(28, 27, 12, 8, 4, 1, 2, 20))}" fill="{hi}" fill-opacity="0.55"/>'
                f'<circle cx="25" cy="31" r="7" fill="{nuc}"/><circle cx="37" cy="28" r="6.5" fill="{nuc}"/><circle cx="34" cy="41" r="6.5" fill="{nuc}"/>'
                f'<path d="M25 31 37 28 34 41Z" fill="{nuc}"/>'
                f'<circle cx="19" cy="44" r="1.8" fill="#fff" fill-opacity="0.8"/><circle cx="45" cy="38" r="1.8" fill="#fff" fill-opacity="0.8"/><circle cx="42" cy="20" r="1.5" fill="#fff" fill-opacity="0.8"/>')
    if key == "cytotoxic_t":
        b = smooth_closed(blob_pts(31, 33, 21, 21, 23, 1.6, 2.0, 36, (8, 10)))
        villi = "".join(f'<path d="M{31 + 21 * math.cos(a):.1f} {33 + 21 * math.sin(a):.1f}l{5 * math.cos(a):.1f} {5 * math.sin(a):.1f}" stroke="{rim}" stroke-width="3"/>'
                        for a in [i * 0.55 - 1.1 for i in range(5)])
        return (villi + f'<path d="{b}" fill="{body}" stroke="{rim}" stroke-width="3"/>'
                f'<path d="{smooth_closed(blob_pts(26, 25, 10, 6, 2, 1, 2, 20))}" fill="{hi}" fill-opacity="0.55"/>'
                f'<circle cx="27" cy="35" r="12" fill="{nuc}"/><ellipse cx="23" cy="31" rx="3.5" ry="2" fill="#fff" fill-opacity="0.35"/>'
                f'<circle cx="44" cy="30" r="3" fill="{RED if not grey else "#9A93A6"}"/><circle cx="45" cy="38" r="2.6" fill="{RED if not grey else "#9A93A6"}"/><circle cx="40" cy="44" r="2.3" fill="{RED if not grey else "#9A93A6"}"/>')
    if key == "macrophage":
        pts = [(10, 30), (4, 18), (14, 14), (24, 16), (34, 6), (42, 12), (40, 22), (54, 24), (60, 34), (50, 40), (46, 52), (34, 56), (24, 50), (12, 54), (8, 44)]
        return (f'<path d="{smooth_closed(pts)}" fill="{body}" stroke="{rim}" stroke-width="3"/>'
                f'<path d="{smooth_closed(blob_pts(24, 24, 9, 5, 7, 1, 2, 20))}" fill="{hi}" fill-opacity="0.55"/>'
                f'<path d="M24 36c0-8 8-12 14-8 4 3 2 8-2 8-5 0-4 6-2 9-5 2-10-3-10-9Z" fill="{nuc}"/>'
                f'<circle cx="42" cy="42" r="8" fill="#ffffff" fill-opacity="0.35" stroke="{rim}" stroke-width="1.6"/>'
                + (bacteria_inner(42, 42, 0.22, 30) if not grey else ""))
    if key == "goblet_cell":
        body_d = "M18 12c-4 10-2 20 6 26v12h16V38c8-6 10-16 6-26-6-4-22-4-28 0Z"
        return (f'<path d="M22 6c1.5 2.4 2.4 3.8 2.4 5a2.4 2.4 0 0 1-4.8 0c0-1.2.9-2.6 2.4-5ZM42 2c1.8 2.8 2.8 4.4 2.8 5.8a2.8 2.8 0 0 1-5.6 0c0-1.4 1-3 2.8-5.8Z" fill="#CFE8FF" stroke="#4E7FB5" stroke-width="1.6"/>'
                f'<path d="{body_d}" fill="{body}" stroke="{rim}" stroke-width="3"/>'
                f'<circle cx="26" cy="18" r="5" fill="#E3F1FF" stroke="#4E7FB5" stroke-width="1.4"/><circle cx="36" cy="16" r="5.5" fill="#E3F1FF" stroke="#4E7FB5" stroke-width="1.4"/>'
                f'<circle cx="31" cy="26" r="5" fill="#E3F1FF" stroke="#4E7FB5" stroke-width="1.4"/><circle cx="40" cy="26" r="3.5" fill="#E3F1FF" stroke="#4E7FB5" stroke-width="1.4"/>'
                f'<ellipse cx="32" cy="44" rx="6" ry="4" fill="{nuc}"/><path d="M14 58h36" stroke="{rim}" stroke-width="3"/>')
    # fibroblast
    return (f'<path d="M4 50c8-3 16-14 26-22s20-14 30-18" stroke="#F28AA0" stroke-width="2.4"/><path d="M8 58c10-3 20-12 30-20s16-12 22-14" stroke="#F28AA0" stroke-width="2.4"/>'
            f'<path d="M6 44C18 30 36 16 58 12 50 26 34 40 6 44Z" fill="{body}" stroke="{rim}" stroke-width="3"/>'
            f'<ellipse cx="32" cy="29" rx="9" ry="4.5" transform="rotate(-28 32 29)" fill="{nuc}"/>'
            f'<ellipse cx="22" cy="34" rx="5" ry="1.8" transform="rotate(-28 22 34)" fill="{hi}" fill-opacity="0.7"/>')


def tower_icon(key, size=72, grey=False):
    return svgwrap(tower_inner(key, grey), size)


# ------------------------------------------------------------------ text helpers
def lab(t, color=INK3, size=13, extra=""):
    return (f'<div style="font-family: {TEXTF}; font-size: {size}px; font-weight: 800; letter-spacing: 0.1em; text-transform: uppercase; '
            f'color: {color}; {extra}">{t}</div>')


def nucleus_key(k, size=26, col=NUC):
    return (f'<span style="display: inline-flex; align-items: center; justify-content: center; min-width: {size}px; height: {size}px; padding: 0 6px; '
            f'box-sizing: border-box; border-radius: {size}px; background: {col}; color: #fff; border: 2px solid {PLUM}; '
            f'font-family: {DISPLAY}; font-weight: 600; font-size: 14px; line-height: 1">{k}</span>')


def keycap(k):
    return (f'<span style="display: inline-flex; align-items: center; justify-content: center; min-width: 26px; height: 26px; padding: 0 7px; '
            f'box-sizing: border-box; border-radius: 9px; background: #fff; border: 2px solid {PLUM}; border-bottom-width: 4px; color: {INK}; '
            f'font-family: {TEXTF}; font-size: 13px; font-weight: 900">{k}</span>')


# ------------------------------------------------------------------ HUD pieces
def organ_panel(v, mem_run=0):
    crit = v < 30
    col = "#F08A9C" if v >= 50 else (AMBER if v >= 30 else RED)
    fill, rim = (FL_FILL, FL_RIM) if not crit else ("#FFE1E1", "#E2475F")
    mw, mh = 300, 26
    fw = (mw - 8) * v / 100
    rbc = ""
    rnd = random.Random(5)
    x = 10
    while x < fw - 4:
        rbc += f'<ellipse cx="{x:.0f}" cy="{mh / 2 + rnd.uniform(-3, 3):.0f}" rx="5" ry="4" fill="#fff" fill-opacity="0.35"/>'
        x += rnd.uniform(11, 17)
    meter = (f'<svg width="{mw}" height="{mh}" viewBox="0 0 {mw} {mh}" aria-hidden="true" style="overflow: visible">'
             f'<rect x="1" y="1" width="{mw - 2}" height="{mh - 2}" rx="{mh / 2 - 1}" fill="#F7D9D3" stroke="{PLUM}" stroke-width="3"/>'
             f'<rect x="4" y="4" width="{fw:.0f}" height="{mh - 8}" rx="{mh / 2 - 4}" fill="{col}"/>{rbc}</svg>')
    inner = (f'<div style="display: flex; flex-direction: column; gap: 6px">{lab("Organ integrity", INK2)}'
             f'<div style="display: flex; align-items: center; gap: 14px">'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 40px; line-height: 1; min-width: 92px; color: {RED if crit else INK}">{v}%</span>{meter}</div></div>')
    return cell_panel(18, 16, 470, 112, inner, "host", seed=21, pad="16px 26px", fill=fill, rim=rim, rimw=8 if crit else 6,
                      cls="anim-throb" if crit else "")


def wave_vessel(wave, total, incoming, on_field, spike=(9,), boss=(12,), prep_secs=None):
    return ""  # no wave bar in the HUD
    """The wave track is a capillary: every wave is a node, the current one swollen."""
    vw, vh = 470, 64
    step = (vw - 40) / (total - 1)
    nodes = ""
    y = 34
    tube = (f'<path d="M14 {y}H{vw - 14}" stroke="{PLUM}" stroke-width="18" stroke-linecap="round"/>'
            f'<path d="M14 {y}H{vw - 14}" stroke="#F4B6AE" stroke-width="11" stroke-linecap="round"/>'
            f'<path d="M14 {y}H{20 + step * (wave - 1):.0f}" stroke="#F08A9C" stroke-width="11" stroke-linecap="round"/>')
    for i in range(1, total + 1):
        x = 20 + step * (i - 1)
        if i < wave:
            nodes += (f'<circle cx="{x:.0f}" cy="{y}" r="9" fill="{LIL_FILL}" stroke="{PLUM}" stroke-width="3"/>'
                      f'<path d="M{x - 4:.0f} {y}l3 3 5-6" stroke="{LIL_DEEP}" stroke-width="2.6" fill="none"/>')
        elif i == wave:
            nodes += (f'<circle class="anim-beat" cx="{x:.0f}" cy="{y}" r="17" fill="#FDE6A0" stroke="{PLUM}" stroke-width="3.5"/>'
                      f'<g>{bacteria_inner(x - 3, y - 3, 0.22, -20)}{virus_inner(x + 5, y + 5, 0.22)}</g>')
        else:
            spk = i in spike
            bs = i in boss
            fillc = "#FFE7C2" if (spk or bs) else "#F9D6CF"
            nodes += f'<circle cx="{x:.0f}" cy="{y}" r="{12 if bs else 8}" fill="{fillc}" stroke="{PLUM}" stroke-width="3"/>'
            if bs:
                nodes += f'<g>{virus_inner(x, y, 0.3, crown=True)}</g>'
            if spk:
                nodes += (f'<text x="{x:.0f}" y="{y - 14}" text-anchor="middle" font-family="Fredoka, sans-serif" font-weight="700" font-size="16" fill="{AMBER_T}">!</text>')
    svg = f'<svg width="{vw}" height="{vh}" viewBox="0 0 {vw} {vh}" aria-hidden="true" style="overflow: visible">{tube}{nodes}</svg>'
    if prep_secs is None:
        sub = (f'<div style="display: flex; gap: 14px; font-family: {TEXTF}; font-size: 14px; font-weight: 700; color: {INK2}; margin-left: 6px">'
               f'<span><b style="color: {INK}">{incoming:,}</b> incoming</span><span><b style="color: {INK}">{on_field:,}</b> on field</span></div>')
    else:
        sub = ""
    inner = (f'<div style="display: flex; align-items: center; gap: 12px">'
             f'<div style="display: flex; flex-direction: column; align-items: center; width: 80px">{lab("Wave", INK2)}'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 44px; line-height: 1; color: {INK}">{wave}</span>'
             f'<span style="font-family: {DISPLAY}; font-weight: 500; font-size: 15px; color: {INK3}">of {total}</span></div>'
             f'<div style="display: flex; flex-direction: column; gap: 0">{svg}{sub}</div></div>')
    return cell_panel((W - 620) // 2, 16, 620, 128, inner, "host", seed=33, pad="12px 22px")


def speed_cells(active="1x"):
    items = [("pause", "Pause"), ("1x", "Normal speed"), ("2x", "Fast"), ("menu", "Menu")]
    out = ""
    for i, (k, aria) in enumerate(items):
        on = k == active
        fill = LIL_DEEP if on else LIL_FILL
        fg = "#fff" if on else INK
        if k == "pause":
            g = f'<rect x="21" y="18" width="7" height="28" rx="3" fill="{fg}"/><rect x="36" y="18" width="7" height="28" rx="3" fill="{fg}"/>'
        elif k == "menu":
            g = f'<path d="M18 22h28M18 32h28M18 42h28" stroke="{fg}" stroke-width="5" stroke-linecap="round"/>'
        else:
            g = f'<text x="32" y="41" text-anchor="middle" font-family="Fredoka, sans-serif" font-weight="600" font-size="24" fill="{fg}">{k[0]}×</text>'
        blob = smooth_closed(blob_pts(32, 32, 26, 25, 50 + i, 1.8, 2.2, 28))
        out += (f'<button aria-label="{aria}" style="width: 56px; height: 56px; padding: 0; border: none; background: transparent; cursor: pointer">'
                f'<svg width="56" height="56" viewBox="0 0 64 64" aria-hidden="true"><path d="{blob}" fill="{fill}" stroke="{PLUM}" stroke-width="3.5"/>{g}</svg></button>')
    return f'<div style="position: absolute; right: 22px; top: 18px; display: flex; gap: 4px">{out}</div>'


def next_wave_card(wave, rows, elite=True, note=None, top=88, glow=False):
    total = sum(r[1] for r in rows)
    body = ""
    for fam, n, _new in rows:
        frac = n / total
        body += (f'<div style="display: flex; align-items: center; gap: 10px">'
                 f'<div style="width: 44px; height: 38px; display: flex; align-items: center; justify-content: center">{germ(fam, 40)}</div>'
                 f'<div style="flex-grow: 1; display: flex; flex-direction: column; gap: 4px">'
                 f'<span style="font-family: {TEXTF}; font-size: 16px; font-weight: 800; color: {INK}">{FAM_NAME[fam]}</span>'
                 f'<div style="height: 9px; border-radius: 5px; background: #F3DDD5; border: 2px solid {PLUM}; overflow: hidden"><div style="width: {frac * 100:.0f}%; height: 100%; background: {FAM_COL[fam]}"></div></div></div>'
                 f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 21px; color: {INK}; min-width: 54px; text-align: right">×{n}</span></div>')
    if elite:
        body += (f'<div style="display: flex; align-items: center; gap: 10px; padding: 4px 10px 4px 4px; border-radius: 18px; background: #FFE9B8; border: 2px dashed {AMBER_T}">'
                 f'<div style="width: 44px; height: 40px; display: flex; align-items: center; justify-content: center">{germ("bacteria", 40, crown=True)}</div>'
                 f'<span style="flex-grow: 1; font-family: {TEXTF}; font-size: 16px; font-weight: 900; color: {AMBER_T}">Elite</span>'
                 f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 21px; color: {AMBER_T}">×1</span></div>')
    inner = (f'<div style="display: flex; flex-direction: column; gap: 9px">'
             f'<div style="display: flex; justify-content: space-between; align-items: baseline">'
             f'<div style="display: flex; flex-direction: column">{lab("Next", INK2)}<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 28px; color: {INK}; line-height: 1.05">Wave {wave}</span></div>'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 18px; color: {INK2}">{total:,}</span></div>{body}</div>')
    h = 116 + 50 * len(rows) + (50 if elite else 0)
    extra = "filter: drop-shadow(0 0 18px rgba(255,236,160,0.9));" if glow else ""
    return cell_panel(18, top, 380, h, inner, "host", seed=44, pad="18px 22px", anchor="right", extra=extra)


def pod(key, atp, state="idle", hk=1):
    _, name, role, cost, _ = TOWER[key]
    if state == "idle" and cost > atp:
        state = "short"
    w, h = 138, 178
    grey = state in ("short", "locked")
    fill = {"armed": "#E4DBFF", "short": "#EEEBF2", "locked": "#EAE7EE"}.get(state, LIL_FILL)
    rim = {"armed": LIL_DEEP, "short": "#BDB4CC", "locked": "#BDB4CC", "hover": "#8C71EA"}.get(state, LIL_RIM)
    lift = {"armed": -16, "hover": -6}.get(state, 0)
    rimw = 9 if state == "armed" else 6
    growth = ""
    if state == "short":
        frac = atp / cost
        growth = (f'<div style="position: absolute; left: 12px; right: 12px; bottom: 10px; height: 12px; border-radius: 7px; background: #fff; border: 2px solid {PLUM}; overflow: hidden">'
                  f'<div style="width: {frac * 100:.0f}%; height: 100%; background: {ATP_C}"></div></div>')
    cost_col = INK if state in ("idle", "hover", "armed") else INK3
    sub = (f'<span style="font-family: {TEXTF}; font-size: 13px; font-weight: 800; color: {ATP_D}">{cost - atp} ATP short</span>' if state == "short" else
           (f'<span style="font-family: {TEXTF}; font-size: 13px; font-weight: 800; color: {INK3}">Not in this level</span>' if state == "locked" else
            ''))
    chip = (f'<span style="position: absolute; top: -12px; left: 50%; transform: translateX(-50%); padding: 3px 10px; border-radius: 999px; background: {LIL_DEEP}; '
            f'border: 2px solid {PLUM}; color: #fff; font-family: {TEXTF}; font-size: 12px; font-weight: 900; letter-spacing: 0.08em; white-space: nowrap">PLACING</span>') if state == "armed" else ""
    cls = ""
    inner = (f'<div style="display: flex; flex-direction: column; align-items: center; gap: 1px; padding-top: 6px">'
             f'<div style="display: flex; justify-content: space-between; align-items: center; width: 100%">{nucleus_key(str(hk), 24, NUC if not grey else "#8C8599")}'
             f'<span style="display: flex; align-items: center; gap: 2px">{mito(22)}<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 18px; color: {cost_col}">{cost}</span></span></div>'
             f'{tower_icon(key, 76, grey)}'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 17px; color: {INK if not grey else INK3}; line-height: 1.1">{name}</span>{sub}</div>')
    return (f'<button{cls} aria-label="Build {name}, {cost} ATP, key {hk}" style="position: relative; width: {w}px; height: {h}px; padding: 0; border: none; '
            f'background: transparent; cursor: pointer; transform: translateY({lift}px); flex-shrink: 0">'
            f'{membrane_svg(w, h, fill, rim, 60 + hk, rimw, 2.6, 3.2, organelles=False)}'
            f'<div style="position: relative; box-sizing: border-box; width: {w}px; height: {h}px; padding: 12px 14px">{inner}</div>{growth}{chip}</button>')


def build_tray(atp, income=4.0, armed=None, x=360):
    groups = {}
    for i, t in enumerate(TOWERS):
        groups.setdefault(t[4], []).append((i + 1, t[0]))
    gh = ""
    for g, items in groups.items():
        pods = "".join(pod(k, atp, "armed" if k == armed else "idle", hk) for hk, k in items)
        gh += (f'<div style="display: flex; flex-direction: column; gap: 6px">{lab(g, INK2, 13, "padding-left: 8px")}'
               f'<div style="display: flex; gap: 8px">{pods}</div></div>')
    atp_block = (f'<div style="width: 160px; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 0">'
                 f'<div class="anim-beat" style="width: 110px; height: 70px; display: flex; align-items: center; justify-content: center">{svgwrap(mito_inner(32, 32, 1.15, -12), 104)}</div>'
                 f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 46px; line-height: 1; color: {INK}">{atp}</span>'
                 f'<span style="font-family: {TEXTF}; font-size: 15px; font-weight: 900; color: {ATP_D}">ATP · +{income:.1f}/s</span></div>')
    inner = f'<div style="display: flex; align-items: flex-end; gap: 18px; height: 100%">{atp_block}{gh}</div>'
    return cell_panel(x, H - 16 - 232, 1000, 232, inner, "immune", seed=77, pad="16px 22px 18px", amp=3.5, n_exp=6)


ABILITIES = [
    ("cascade", "Cascade", "Complement Cascade Burst", 90, GOLD, "Q"),
    ("histamine", "Histamine", "Histamine Flare", 45, "#E0559A", "W"),
    ("fever", "Fever", "Fever Response", 60, "#F26A3D", "E"),
    ("clot", "Clot", "Fibrin Clot", 75, "#D7334A", "R"),
]


def ability_inner(key):
    if key == "cascade":
        return (f'<path d="M14 44 26 34 38 38 50 22" stroke="{GOLD_D}" stroke-width="4"/>'
                + "".join(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{GOLD}" stroke="{GOLD_D}" stroke-width="3"/>' for x, y, r in [(14, 44, 7), (26, 34, 6), (38, 38, 6.5), (50, 22, 8)])
                + f'<path d="M50 10v5M58 16l-4 3M42 13l3 4" stroke="{GOLD_D}" stroke-width="3"/>')
    if key == "histamine":
        out = f'<circle cx="32" cy="32" r="9" fill="#E0559A" stroke="#7A1747" stroke-width="3"/>'
        for i in range(8):
            a = i / 8 * 2 * math.pi
            out += f'<circle cx="{32 + 19 * math.cos(a):.1f}" cy="{32 + 19 * math.sin(a):.1f}" r="{4.6 if i % 2 else 3.4}" fill="#F48CC0" stroke="#7A1747" stroke-width="2.4"/>'
        return out
    if key == "fever":
        return (f'<path d="M38 36V14a6 6 0 0 0-12 0v22a10 10 0 1 0 12 0Z" fill="#fff" stroke="#7A2410" stroke-width="3.2"/>'
                f'<circle cx="32" cy="44" r="6.5" fill="#F26A3D"/><path d="M32 42V20" stroke="#F26A3D" stroke-width="5"/>'
                f'<path d="M46 16c3 3-1 6 2 9M52 12c3 3-1 6 2 9" stroke="#F26A3D" stroke-width="3"/>')
    return (f'<path d="M8 20 56 44M8 34l44 20M10 48l46-30M14 12l36 44M30 8 22 56" stroke="#C9A77A" stroke-width="3"/>'
            + "".join(f'<ellipse cx="{x}" cy="{y}" rx="8" ry="6.5" fill="#D7334A" stroke="#6E0F1E" stroke-width="2.6"/><ellipse cx="{x}" cy="{y}" rx="3.4" ry="2.4" fill="#9E1A2E"/>'
                      for x, y in [(22, 26), (40, 36), (28, 46)]))


def vesicle(ab, state, remaining=0, size=92):
    key, short, full, cd, col, hk = ab
    cid = uid("vc")
    frac = 1.0 if state != "cooling" else (cd - remaining) / cd
    r = 40
    level = 50 + r - 2 * r * frac
    wave = f"M0 {level:.1f} q12 -6 25 0 t25 0 t25 0 t25 0 V100 H0Z"
    icon_op = 1 if state != "cooling" else 0.45
    ring = PLUM
    halo = ""
    if state == "ready":
        halo = f'<circle cx="50" cy="50" r="48" fill="none" stroke="{col}" stroke-width="4" stroke-opacity="0.55" class="anim-halo"/>'
    if state == "armed":
        halo = (f'<circle cx="50" cy="50" r="49" fill="none" stroke="{col}" stroke-width="6" class="anim-halo"/>')
    txt = ""
    if state == "cooling":
        txt = (f'<text x="50" y="60" text-anchor="middle" font-family="Fredoka, sans-serif" font-weight="600" font-size="28" fill="{INK}" '
               f'stroke="#fff" stroke-width="5" paint-order="stroke">{remaining}s</text>')
    blob = smooth_closed(blob_pts(50, 50, 42, 41, sum(map(ord, key)) % 97, 1.6, 2.0, 30))
    svg = (f'<svg width="{size}" height="{size}" viewBox="0 0 100 100" aria-hidden="true" style="overflow: visible">'
           f'<defs><clipPath id="{cid}"><path d="{blob}"/></clipPath></defs>{halo}'
           f'<path d="{blob}" fill="#FBF8FF"/>'
           f'<g clip-path="url(#{cid})"><path d="{wave}" fill="{col}" fill-opacity="0.38"/>'
           + "".join(f'<circle cx="{gx}" cy="{gy}" r="3" fill="{col}" fill-opacity="0.55"/>' for gx, gy in [(24, 74), (40, 82), (62, 78), (76, 68), (52, 90)] if gy > level)
           + f'</g><path d="{blob}" fill="none" stroke="{col}" stroke-width="6"/><path d="{blob}" fill="none" stroke="{ring}" stroke-width="3" transform="translate(50 50) scale(1.07) translate(-50 -50)"/>'
           f'<g transform="translate(20 20) scale(0.94)" opacity="{icon_op}">{ability_inner(key)}</g>{txt}</svg>')
    chip = (f'<span style="position: absolute; top: -10px; left: 50%; transform: translateX(-50%); padding: 2px 9px; border-radius: 999px; background: {col}; border: 2px solid {PLUM}; '
            f'color: #fff; font-family: {TEXTF}; font-size: 12px; font-weight: 900; letter-spacing: 0.08em; white-space: nowrap">AIMING</span>') if state == "armed" else ""
    cls = ' class="anim-wobble"' if state in ("ready", "armed") else ""
    btn = (f'<button{cls} aria-label="{full}, key {hk}" style="position: relative; width: {size}px; height: {size}px; padding: 0; border: none; background: transparent; cursor: pointer">{svg}{chip}</button>')
    return (f'<div style="display: flex; flex-direction: column; align-items: center; gap: 4px">{btn}'
            f'<div style="display: flex; align-items: center; gap: 5px">{keycap(hk)}<span style="font-family: {TEXTF}; font-size: 14px; font-weight: 800; color: {INK if state != "cooling" else INK3}">{short}</span></div></div>')


def ability_tray(states):
    items = "".join(vesicle(ab, s, r) for ab, (s, r) in zip(ABILITIES, states))
    inner = f'<div style="display: flex; justify-content: space-between; align-items: center; height: 100%">{items}</div>'
    return cell_panel(18, H - 16 - 156, 464, 156, inner, "immune", seed=88, pad="10px 20px", anchor="right", n_exp=4)


def hint_bar(items, x_center=860, bottom=262, col=LIL_DEEP):
    parts = ""
    for i, (keys, txt) in enumerate(items):
        k = "".join(keycap(kk) + (f'<span style="color: {INK3}; font-weight: 900; margin: 0 2px">/</span>' if j < len(keys) - 1 else "") for j, kk in enumerate(keys))
        parts += (f'<div style="display: flex; align-items: center; gap: 6px">{k}<span style="font-family: {TEXTF}; font-size: 15px; font-weight: 800; color: {INK}">{txt}</span></div>')
        if i < len(items) - 1:
            parts += f'<span style="width: 7px; height: 7px; border-radius: 50%; background: {col}"></span>'
    w = 150 + 140 * len(items)
    inner = f'<div style="display: flex; align-items: center; justify-content: center; gap: 14px; height: 100%">{parts}</div>'
    return cell_panel(x_center - w // 2, H - bottom - 58, w, 58, inner, "immune", seed=99, pad="0 20px", n_exp=3, amp=2, rim=col)


def bubble(x, y, html, rim=LIL_RIM, w=280, h=50, seed=5, fill=LIL_FILL):
    inner = f'<div style="display: flex; align-items: center; gap: 8px; height: 100%; font-family: {TEXTF}; font-size: 15px; font-weight: 800; color: {INK}; white-space: nowrap">{html}</div>'
    return cell_panel(x, y, w, h, inner, "immune", seed=seed, pad="0 16px", rimw=4, n_exp=3, amp=1.6, rim=rim, fill=fill)


# ------------------------------------------------------------------ world
SEGS = [((-60, 300), (220, 300), (360, 262), (500, 340)),
        ((500, 340), (660, 430), (590, 650), (760, 710)),
        ((760, 710), (930, 770), (1010, 600), (1070, 480)),
        ((1070, 480), (1130, 360), (1290, 320), (1380, 430)),
        ((1380, 430), (1450, 520), (1520, 575), (1630, 575))]
ORGAN = (1700, 575)
LANE_HALF = 88


def bez(sg, t):
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = sg
    u = 1 - t
    return (u ** 3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t ** 3 * x3,
            u ** 3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t ** 3 * y3)


SAMPLES = [bez(sg, i / 160) for sg in SEGS for i in range(160)] + [SEGS[-1][3]]
CUM = [0.0]
for a_, b_ in zip(SAMPLES, SAMPLES[1:]):
    CUM.append(CUM[-1] + math.dist(a_, b_))
LEN = CUM[-1]


def lane_at(s):
    tgt = s * LEN
    lo = 0
    while lo < len(CUM) - 2 and CUM[lo + 1] < tgt:
        lo += 1
    a, b = SAMPLES[lo], SAMPLES[lo + 1]
    f = (tgt - CUM[lo]) / max(1e-6, CUM[lo + 1] - CUM[lo])
    dx, dy = b[0] - a[0], b[1] - a[1]
    d = math.hypot(dx, dy) or 1
    return (a[0] + dx * f, a[1] + dy * f), (-dy / d, dx / d), math.degrees(math.atan2(dy, dx))


def lane_dist(p):
    return min(math.dist(p, q) for q in SAMPLES)


def path_d(segs=SEGS):
    d = f"M{segs[0][0][0]} {segs[0][0][1]}"
    for _, c1, c2, e in segs:
        d += f" C{c1[0]} {c1[1]} {c2[0]} {c2[1]} {e[0]} {e[1]}"
    return d


ZONES = [((640, 500), 150), ((930, 600), 120), ((1230, 520), 130), ((450, 470), 110)]


def in_zone(x, y):
    return any(math.dist((x, y), c) < r and lane_dist((x, y)) > 70 for c, r in ZONES)


def tissue(zones=False):
    rnd = random.Random(8)
    out = ""
    sx, sy = 132, 114
    for gy in range(-1, int(H / sy) + 2):
        for gx in range(-1, int(W / sx) + 2):
            cx = gx * sx + (sx / 2 if gy % 2 else 0) + rnd.uniform(-14, 14)
            cy = gy * sy + rnd.uniform(-12, 12)
            a, b = rnd.uniform(52, 60), rnd.uniform(46, 54)
            pts = blob_pts(cx, cy, a, b, rnd.randint(0, 9999), 5, 2.6, 14, (2, 3))
            hot = False
            fill = "#7A2A5A" if hot else CELL
            out += f'<path d="{smooth_closed(pts)}" fill="{fill}"/>'
            if hot:
                out += f'<path d="{smooth_closed(pts)}" fill="none" stroke="{BODY_HI}" stroke-width="3" stroke-dasharray="2 7" stroke-linecap="round"/>'
            nx, ny = cx + rnd.uniform(-14, 14), cy + rnd.uniform(-12, 12)
            out += (f'<ellipse cx="{nx:.0f}" cy="{ny:.0f}" rx="{rnd.uniform(15, 20):.0f}" ry="{rnd.uniform(11, 15):.0f}" fill="{"#8E3C74" if hot else CELL_NUC}"/>'
                    f'<circle cx="{nx + rnd.uniform(-5, 5):.0f}" cy="{ny + rnd.uniform(-4, 4):.0f}" r="{rnd.uniform(4, 6):.1f}" fill="{"#A65A92" if hot else CELL_NUC2}"/>')
            if rnd.random() < 0.5:
                out += f'<circle cx="{cx + rnd.uniform(-30, 30):.0f}" cy="{cy + rnd.uniform(-26, 26):.0f}" r="{rnd.uniform(3, 5):.1f}" fill="{CELL_NUC}"/>'
    return f'<rect width="{W}" height="{H}" fill="{MATRIX}"/>{out}'


ZONE_RECTS = [(470, 330, 640, 470), (1080, 280, 380, 360)]


def zone_svg():
    d = path_d()
    clip = "".join(f'<rect x="{x}" y="{y}" width="{w}" height="{h}"/>' for x, y, w, h in ZONE_RECTS)
    band = 2 * LANE_HALF - 30
    return (f'<defs><clipPath id="zones">{clip}</clipPath>'
            f'<pattern id="buildhatch" width="16" height="16" patternUnits="userSpaceOnUse" patternTransform="rotate(40)">'
            f'<rect width="16" height="16" fill="{BODY_HI}" fill-opacity="0.35"/><rect width="6" height="16" fill="{BODY}" fill-opacity="0.55"/></pattern></defs>'
            f'<g clip-path="url(#zones)"><path d="{d}" stroke="#ffffff" stroke-width="{band + 6}" fill="none"/>'
            f'<path d="{d}" stroke="{LANE_IN}" stroke-width="{band}" fill="none"/>'
            f'<path d="{d}" stroke="url(#buildhatch)" stroke-width="{band}" fill="none"/></g>')


def lane_svg():
    d = path_d()
    rnd = random.Random(12)
    specks = ""
    for i in range(140):
        (x, y), (nx, ny), _ = lane_at(rnd.random())
        o = rnd.uniform(-0.8, 0.8) * LANE_HALF
        pts = blob_pts(x + nx * o, y + ny * o, rnd.uniform(9, 16), rnd.uniform(7, 12), i, 1.5, 2.4, 12, (2, 3))
        specks += f'<path d="{smooth_closed(pts)}" fill="#F3A99A" fill-opacity="0.45"/>'
    return (f'<path d="{d}" stroke="{LANE_RIM}" stroke-width="210" fill="none" stroke-linecap="round"/>'
            f'<path d="{d}" stroke="{LANE_WALL}" stroke-width="196" fill="none" stroke-linecap="round"/>'
            f'<path d="{d}" stroke="{LANE_IN}" stroke-width="176" fill="none" stroke-linecap="round"/>'
            f'<path d="{d}" stroke="{LANE_CORE}" stroke-opacity="0.8" stroke-width="80" fill="none" stroke-linecap="round"/>{specks}'
            f'<ellipse cx="0" cy="300" rx="40" ry="96" fill="#3A0A20" stroke="{LANE_RIM}" stroke-width="6"/>')


def organ_world(v):
    return ""


def horde(s0, s1, n, seed, mix=(0.6, 0.34, 0.06), pack=0.4, elite_at=None):
    rnd = random.Random(seed)
    glow, bits = "", ""
    for i in range(n):
        u = rnd.random() ** (1 - pack)
        (x, y), (nx, ny), ang = lane_at(s0 + (s1 - s0) * u)
        o = max(-0.85, min(0.85, rnd.gauss(0, 0.42))) * LANE_HALF
        x, y = x + nx * o, y + ny * o
        r = rnd.random()
        if r < mix[0]:
            a = ang + rnd.uniform(-25, 25)
            bits += (f'<g transform="translate({x:.0f} {y:.0f}) rotate({a:.0f})"><path d="M-8 0c-4 -3 -6 3 -10 0" stroke="{BACT_D}" stroke-width="1.4" fill="none"/>'
                     f'<rect x="-8" y="-3.6" width="16" height="7.2" rx="3.6" fill="{BACT}" stroke="{BACT_D}" stroke-width="1.4"/></g>')
            if i % 4 == 0:
                glow += f'<circle cx="{x:.0f}" cy="{y:.0f}" r="18" fill="{BACT}" fill-opacity="0.35"/>'
        elif r < mix[0] + mix[1]:
            bits += (f'<circle cx="{x:.0f}" cy="{y:.0f}" r="6.5" fill="none" stroke="{VIRUS_D}" stroke-width="3" stroke-dasharray="1.6 2.4"/>'
                     f'<circle cx="{x:.0f}" cy="{y:.0f}" r="4.6" fill="{VIRUS}" stroke="{VIRUS_D}" stroke-width="1.3"/>')
            if i % 4 == 0:
                glow += f'<circle cx="{x:.0f}" cy="{y:.0f}" r="16" fill="{VIRUS}" fill-opacity="0.3"/>'
        else:
            a = rnd.uniform(0, 180)
            bits += (f'<g transform="translate({x:.0f} {y:.0f}) rotate({a:.0f})"><path d="M-12 3c4-9 8 4 12-3s8 3 12-5" stroke="{PARA_D}" stroke-width="8" fill="none" stroke-linecap="round"/>'
                     f'<path d="M-12 3c4-9 8 4 12-3s8 3 12-5" stroke="{PARA}" stroke-width="5" fill="none" stroke-linecap="round"/></g>')
    el = ""
    if elite_at is not None:
        (x, y), _, _ = lane_at(elite_at)
        el = (f'<circle cx="{x:.0f}" cy="{y:.0f}" r="30" fill="#FFE39A" fill-opacity="0.55"/>'
              f'{bacteria_inner(x, y, 0.75, -10, crown=True)}')
    return f'<g filter="url(#soft)">{glow}</g>{bits}{el}'


def tower_world(key, x, y, hp=1.0, sel=False, ring=0, ring_mode="faint", latched=0, ghost=False):
    out = ""
    if ring:
        if ring_mode == "sel":
            out += (f'<circle cx="{x}" cy="{y}" r="{ring}" fill="{BODY_HI}" fill-opacity="0.22" stroke="{LIL_FILL}" stroke-width="4" stroke-dasharray="1 12" stroke-linecap="round"/>'
                    f'<circle cx="{x}" cy="{y}" r="{ring}" fill="none" stroke="{BODY}" stroke-width="2"/>')
        elif ring_mode == "ghost":
            out += (f'<circle cx="{x}" cy="{y}" r="{ring}" fill="{BODY_HI}" fill-opacity="0.28" stroke="#fff" stroke-width="4" stroke-dasharray="1 12" stroke-linecap="round"/>')
        else:
            out += f'<circle cx="{x}" cy="{y}" r="{ring}" fill="{BODY_HI}" fill-opacity="0.10" stroke="{BODY_HI}" stroke-opacity="0.7" stroke-width="2.5" stroke-dasharray="1 10" stroke-linecap="round"/>'
    if sel:
        out += f'<circle class="anim-spin" cx="{x}" cy="{y}" r="38" fill="none" stroke="#fff" stroke-width="4" stroke-dasharray="10 9" stroke-linecap="round"/>'
    op = ' opacity="0.7"' if ghost else ""
    out += f'<ellipse cx="{x}" cy="{y + 22}" rx="24" ry="7" fill="#2A0012" fill-opacity="0.35"/>'
    out += f'<g{op} transform="translate({x - 27} {y - 27}) scale(0.84)">{tower_inner(key)}</g>'
    if hp < 1:
        col = "#8FE08A" if hp > 0.66 else (AMBER if hp > 0.33 else RED)
        out += (f'<rect x="{x - 28}" y="{y + 32}" width="56" height="10" rx="5" fill="#fff" stroke="{PLUM}" stroke-width="2.5"/>'
                f'<rect x="{x - 25}" y="{y + 35}" width="{50 * hp:.0f}" height="4" rx="2" fill="{col}"/>')
    rnd = random.Random(x * 3 + y)
    for _ in range(latched):
        a = rnd.uniform(0, 2 * math.pi)
        out += f'<g>{virus_inner(x + 26 * math.cos(a), y + 26 * math.sin(a), 0.32)}</g>'
    return out


def swarmers(src, dst, n, seed):
    rnd = random.Random(seed)
    out = ""
    for i in range(n):
        f = rnd.uniform(0.3, 0.95)
        x = src[0] + (dst[0] - src[0]) * f + rnd.gauss(0, 18)
        y = src[1] + (dst[1] - src[1]) * f + rnd.gauss(0, 18)
        b = smooth_closed(blob_pts(x, y, 8, 7, i + seed, 1.2, 2, 12, (3, 4)))
        out += (f'<path d="{b}" fill="{BODY_HI}" stroke="{LIL_DEEP}" stroke-width="1.6"/>'
                f'<circle cx="{x - 2:.0f}" cy="{y:.0f}" r="2.4" fill="{NUC}"/><circle cx="{x + 2:.0f}" cy="{y - 1:.0f}" r="2.2" fill="{NUC}"/><circle cx="{x + 1:.0f}" cy="{y + 2.5:.0f}" r="2.2" fill="{NUC}"/>')
    return out


def world(content, v=72, zones=False, dim=0.0, inflamed=False):
    extra = f'<rect width="{W}" height="{H}" fill="#2A0012" fill-opacity="{dim}"/>' if dim else ""
    infl = ""
    if inflamed:
        infl = (f'<defs><radialGradient id="infl" cx="50%" cy="50%" r="72%"><stop offset="0.55" stop-color="#FF2A4A" stop-opacity="0"/>'
                f'<stop offset="1" stop-color="#FF2A4A" stop-opacity="0.55"/></radialGradient></defs>'
                f'<rect class="anim-pulse" width="{W}" height="{H}" fill="url(#infl)"/>')
    return (f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" style="position: absolute; left: 0; top: 0" aria-hidden="true">'
            f'<defs><filter id="soft" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="9"/></filter></defs>'
            f'{tissue(zones)}{lane_svg()}{zone_svg() if zones else ""}{organ_world(v)}{content}{extra}{infl}</svg>')


# ------------------------------------------------------------------ page
HELMET = f"""<helmet>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Fredoka:wght@500;600;700&amp;family=Nunito:wght@600;700;800;900&amp;display=swap">
<style>
body{{margin:0;background:{MATRIX};font-family:{TEXTF};color:{INK}}}
button{{font:inherit}}
@keyframes beat{{0%,100%{{transform:scale(1)}}12%{{transform:scale(1.06)}}24%{{transform:scale(0.99)}}36%{{transform:scale(1.04)}}}}
@keyframes wobble{{0%,100%{{transform:rotate(-1.5deg) scale(1)}}50%{{transform:rotate(1.5deg) scale(1.03)}}}}
@keyframes halo{{0%,100%{{opacity:0.35}}50%{{opacity:1}}}}
@keyframes pulse{{0%,100%{{opacity:1}}50%{{opacity:0.5}}}}
@keyframes spin{{to{{transform:rotate(360deg)}}}}
@keyframes throb{{0%,100%{{transform:scale(1)}}50%{{transform:scale(1.015)}}}}
.anim-beat{{transform-box:fill-box;transform-origin:center;animation:beat 1.4s ease-in-out infinite}}
.anim-beat-fast{{transform-box:fill-box;transform-origin:center;animation:beat 0.6s ease-in-out infinite}}
.anim-halo{{animation:halo 1.6s ease-in-out infinite}}
.anim-pulse{{animation:pulse 1s ease-in-out infinite}}
.anim-spin{{transform-box:fill-box;transform-origin:center;animation:spin 8s linear infinite}}
.anim-throb{{animation:throb 0.6s ease-in-out infinite}}
.anim-wobble{{animation:wobble 2.2s ease-in-out infinite}}
</style>
</helmet>"""


def page(title, body, w=W, h=H, bg=MATRIX):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="./support.js"></script>
</head>
<body>
<x-dc>
{HELMET}
<div style="position: relative; width: {w}px; height: {h}px; overflow: hidden; background: {bg}; font-family: {TEXTF}; color: {INK}">
{body}
</div>
</x-dc>
<script type="text/x-dc" data-dc-script data-props='{{"$preview":{{"width":{w},"height":{h}}}}}'>
class Component extends DCLogic {{
renderVals() {{
return {{}};
}}
}}
</script>
</body>
</html>
"""


# ------------------------------------------------------------------ scenes
OVERLAP_ICON = (f'<circle cx="24" cy="32" r="16" fill="{BODY_HI}" stroke="{LIL_DEEP}" stroke-width="3"/>'
                f'<circle cx="40" cy="32" r="16" fill="{BODY_HI}" fill-opacity="0.7" stroke="{LIL_DEEP}" stroke-width="3"/>')
def lane_pt(t, off):
    (x, y), (nx, ny), _ = lane_at(t)
    return round(x + nx * off * LANE_HALF), round(y + ny * off * LANE_HALF)


TW = [("neutrophil", lane_pt(0.13, 0.3)), ("cytotoxic_t", lane_pt(0.36, -0.3)), ("macrophage", lane_pt(0.25, -0.15)),
      ("goblet_cell", lane_pt(0.55, 0.3)), ("neutrophil", lane_pt(0.66, -0.3)), ("fibroblast", lane_pt(0.82, 0.25))]
RING = 150
ROWS8 = [("bacteria", 320, False), ("virus", 180, False), ("parasite", 40, True)]


def scene_main():
    t = "".join(tower_world(k, *p, hp=(0.55 if k == "cytotoxic_t" else 1)) for k, p in TW)
    c = horde(0.0, 0.40, 520, 11, pack=0.45, elite_at=0.30) + t + swarmers(TW[1][1], lane_at(0.40)[0], 8, 4) + swarmers(TW[0][1], lane_at(0.2)[0], 6, 5)
    return page("IMMUNE HUD, wave in progress",
                world(c, 72) + organ_panel(72) + wave_vessel(7, 12, 412, 3108) + speed_cells("1x") + next_wave_card(8, ROWS8) +
                build_tray(142) + ability_tray([("cooling", 62), ("ready", 0), ("cooling", 14), ("cooling", 40)]))


ZB = (150, 470)


def scene_place():
    gx, gy = lane_pt(0.46, 0.05)
    t = "".join(tower_world(k, *p, ring=RING) for k, p in TW)
    ghost = tower_world("cytotoxic_t", gx, gy, ring=RING, ring_mode="ghost", ghost=True)
    ghost += f'<path d="M{gx + 40} {gy + 38}l0 26 7-7 6 13 5-2-6-13h10Z" fill="#fff" stroke="{PLUM}" stroke-width="2.5" stroke-linejoin="round"/>'
    c = horde(0.0, 0.22, 260, 21, pack=0.3) + t + ghost
    b = bubble(gx - 330, gy - 25, f'{mito(24)}<span style="font-family: {DISPLAY}; font-size: 20px; font-weight: 600">−130</span>'
                                  f'<span style="color: {INK3}">→</span><span>12 left</span>', ATP_C, 230, 50, 3)
    return page("IMMUNE HUD, placing a tower",
                world(c, 72, zones=True) + b + organ_panel(72) + wave_vessel(7, 12, 388, 2940) + speed_cells("1x") + next_wave_card(8, ROWS8) +
                hint_bar([(["LMB"], "Place"), (["RMB", "Esc"], "Cancel")]) +
                build_tray(142, armed="cytotoxic_t") + ability_tray([("cooling", 58), ("ready", 0), ("cooling", 10), ("cooling", 36)]))


def inspector(tx, ty):
    key = "cytotoxic_t"
    _, name, role, cost, _ = TOWER[key]
    pw, ph = 330, 188
    px, py = tx + 80, ty - 260
    hp = 214 / 380
    inner = (f'<div style="display: flex; flex-direction: column; gap: 12px">'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 28px; line-height: 1; color: {INK}">{name}</span>'
             f'<div style="display: flex; align-items: center; gap: 10px">'
             f'<div style="flex-grow: 1; height: 16px; border-radius: 9px; background: #fff; border: 2.5px solid {PLUM}; overflow: hidden">'
             f'<div style="width: {hp * 100:.0f}%; height: 100%; background: {AMBER}"></div></div>'
             f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 18px; color: {INK}">214 / 380</span></div>'
             f'<button style="height: 52px; border-radius: 18px; border: 3px solid {PLUM}; background: #FFE3CF; cursor: pointer; display: flex; align-items: center; '
             f'justify-content: center; gap: 10px; font-family: {DISPLAY}; font-weight: 600; font-size: 22px; color: {INK}">Sell '
             f'<span style="display: flex; align-items: center; gap: 3px; color: {ATP_D}">+{mito(24)}{round(cost * 0.7)}</span></button></div>')
    tether = (f'<svg width="{W}" height="{H}" style="position: absolute; left: 0; top: 0; pointer-events: none" aria-hidden="true">'
              f'<path d="M{tx + 16} {ty - 28}C{tx + 30} {ty - 60} {px + 30} {py + ph + 10} {px + 60} {py + ph - 10}" stroke="{PLUM}" stroke-width="15" fill="none" stroke-linecap="round"/>'
              f'<path d="M{tx + 16} {ty - 28}C{tx + 30} {ty - 60} {px + 30} {py + ph + 10} {px + 60} {py + ph - 10}" stroke="{LIL_RIM}" stroke-width="9" fill="none" stroke-linecap="round"/></svg>')
    return tether + cell_panel(px, py, pw, ph, inner, "immune", seed=15, pad="20px 24px", rim=LIL_DEEP, rimw=7, n_exp=4)


def scene_inspect():
    sel = TW[1][1]
    t = ""
    for k, p in TW:
        t += tower_world(k, *p, hp=214 / 380, sel=True, ring=RING, ring_mode="sel", latched=3) if p == sel else tower_world(k, *p)
    c = horde(0.12, 0.52, 460, 31, pack=0.4) + t + swarmers(sel, lane_at(0.46)[0], 8, 9)
    return page("IMMUNE HUD, tower selected",
                world(c, 64, dim=0.18) + inspector(*sel) + organ_panel(64, 41) + wave_vessel(7, 12, 206, 3380) + speed_cells("1x") +
                next_wave_card(8, ROWS8) + build_tray(96) + ability_tray([("cooling", 44), ("ready", 0), ("ready", 0), ("cooling", 22)]))


def prep_timer():
    secs = 18
    p = secs / 30
    r = 44
    c = 2 * math.pi * r
    ring = (f'<svg width="104" height="104" viewBox="0 0 104 104" aria-hidden="true">'
            f'<circle cx="52" cy="52" r="{r}" fill="#fff" stroke="{PLUM}" stroke-width="14"/><circle cx="52" cy="52" r="{r}" fill="none" stroke="#F4D3CC" stroke-width="8"/>'
            f'<circle cx="52" cy="52" r="{r}" fill="none" stroke="{LIL_DEEP}" stroke-width="8" stroke-linecap="round" stroke-dasharray="{c * p:.1f} {c:.1f}" transform="rotate(-90 52 52)"/>'
            f'<text x="52" y="62" text-anchor="middle" font-family="Fredoka, sans-serif" font-weight="600" font-size="28" fill="{INK}">0:{secs}</text></svg>')
    inner = (f'<div style="display: flex; align-items: center; gap: 16px; height: 100%">{ring}'
             f'<span style="flex-grow: 1; font-family: {DISPLAY}; font-weight: 600; font-size: 30px; color: {INK}">Wave 8</span>'
             f'<button style="height: 60px; padding: 0 18px; border-radius: 22px; border: 3px solid {PLUM}; background: {LIL_DEEP}; color: #fff; cursor: pointer; '
             f'display: flex; align-items: center; gap: 10px; font-family: {DISPLAY}; font-weight: 600; font-size: 19px">Send now {keycap("Space")}</button></div>')
    return cell_panel((W - 480) // 2, 16, 480, 136, inner, "immune", seed=71, pad="14px 20px")


def scene_prep():
    t = "".join(tower_world(k, *p, ring=RING) for k, p in TW)
    chev = "".join(f'<path class="anim-pulse" d="M{60 + i * 34} 266l24 34-24 34" stroke="#FFE39A" stroke-width="10" fill="none" stroke-linecap="round" stroke-linejoin="round" opacity="{0.5 + i * 0.25}"/>' for i in range(3))
    c = t + chev
    entering = "".join(f'<span style="display: flex; align-items: center; gap: 3px">{germ(f, 30)}<span style="font-family: {DISPLAY}; font-size: 18px; font-weight: 600">×{n}</span></span>' for f, n, _ in ROWS8)
    beacon = cell_panel(28, 410, 290, 64, f'<div style="display: flex; gap: 10px; align-items: center; height: 100%">{entering}</div>',
                        "host", seed=81, pad="0 18px", rim=AMBER, fill="#FFF6DD")
    cleared = cell_panel((W - 340) // 2, 166, 340, 64,
                         f'<div style="display: flex; align-items: center; justify-content: center; height: 100%">'
                         f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 24px">Wave 7 cleared</span></div>', "immune", seed=82, pad="0 18px", n_exp=3)
    return page("IMMUNE HUD, prep between waves",
                world(c, 64) + beacon + organ_panel(64) + wave_vessel(8, 12, 0, 0, prep_secs=18) + prep_timer() + cleared +
                next_wave_card(8, ROWS8, glow=True) + build_tray(231) + ability_tray([("cooling", 30), ("ready", 0), ("ready", 0), ("ready", 0)]))


def scene_critical():
    t = "".join(tower_world(k, *p, hp=(0.3 if k in ("goblet_cell", "fibroblast") else 0.8)) for k, p in TW)
    (tx, ty), _, _ = lane_at(0.9)
    tx, ty = round(tx), round(ty)
    aim = (f'<circle cx="{tx}" cy="{ty}" r="140" fill="#E0559A" fill-opacity="0.22" stroke="#fff" stroke-width="5" stroke-dasharray="1 13" stroke-linecap="round"/>'
           f'<circle cx="{tx}" cy="{ty}" r="140" fill="none" stroke="#E0559A" stroke-width="3"/>'
           + "".join(f'<circle cx="{tx + 110 * math.cos(a):.0f}" cy="{ty + 110 * math.sin(a):.0f}" r="7" fill="#F48CC0" stroke="#7A1747" stroke-width="2.5"/>' for a in [i * math.pi / 4 for i in range(8)]))
    c = horde(0.52, 0.965, 640, 41, mix=(0.45, 0.45, 0.10), pack=0.65, elite_at=0.80) + t + aim
    banner = cell_panel((W - 320) // 2, 24, 320, 70,
                        f'<div style="display: flex; align-items: center; justify-content: center; gap: 12px; height: 100%">'
                        f'<span style="font-family: {DISPLAY}; font-weight: 700; font-size: 26px; color: {RED}">Organ failing</span></div>',
                        "host", seed=91, pad="0 20px", fill="#FFE1E1", rim=RED, rimw=8, n_exp=3, cls="anim-throb")
    caught = bubble(tx - 150, ty + 158, f'{svgwrap(ability_inner("histamine"), 26)}<b style="color: #B0246E">Histamine Flare</b><span style="color: {INK2}">≈ 210 caught</span>', "#E0559A", 330, 50, 8)
    return page("IMMUNE HUD, organ critical",
                world(c, 18, inflamed=True) + banner + organ_panel(18, 52) + wave_vessel(11, 12, 96, 4212) + speed_cells("1x") +
                next_wave_card(12, [("bacteria", 380, False), ("virus", 260, False), ("parasite", 90, False)]) +
                hint_bar([(["LMB"], "Cast"), (["RMB", "Esc"], "Cancel")], col="#E0559A") +
                build_tray(58) + ability_tray([("ready", 0), ("armed", 0), ("cooling", 26), ("cooling", 51)]))


# ------------------------------------------------------------------ kit
def kit_block(x, y, w, h, title, inner, kind="immune", seed=1):
    body = (f'<div style="display: flex; flex-direction: column; gap: 14px">'
            f'<span style="font-family: {DISPLAY}; font-weight: 600; font-size: 24px; color: {INK}">{title}</span>{inner}</div>')
    return cell_panel(x, y, w, h, body, kind, seed=seed, pad="22px 28px", n_exp=6)


def cap(t):
    return f'<span style="font-family: {TEXTF}; font-size: 14px; font-weight: 800; color: {INK2}; text-align: center">{t}</span>'


def scene_kit():
    kw, kh = 1920, 1240
    sw = [("Plum outline", PLUM), ("Immune cytoplasm", LIL_FILL), ("Immune membrane", LIL_RIM), ("Nucleus", NUC), ("Host flesh", FL_FILL),
          ("Host membrane", FL_RIM), ("ATP", ATP_C), ("Caution", AMBER), ("Critical", RED)]
    swatches = "".join(
        f'<div style="display: flex; align-items: center; gap: 10px"><div style="width: 46px; height: 46px; border-radius: 45% 55% 50% 50%; background: {c}; border: 3px solid {PLUM}; flex-shrink: 0"></div>'
        f'<div style="display: flex; flex-direction: column"><span style="font-size: 15px; font-weight: 900">{n}</span><span style="font-size: 13px; font-weight: 700; color: {INK3}">{c}</span></div></div>' for n, c in sw)
    germs = "".join(f'<div style="display: flex; flex-direction: column; align-items: center; gap: 4px">{germ(f, 64, crown=cr)}{cap(t)}</div>'
                    for f, cr, t in [("bacteria", False, "Bacteria"), ("virus", False, "Virus"), ("parasite", False, "Parasite"), ("bacteria", True, "Elite")])
    towers = "".join(f'<div style="display: flex; flex-direction: column; align-items: center; gap: 2px">{tower_icon(k, 84)}{cap(n)}</div>' for k, n, *_ in TOWERS)
    pods = "".join(f'<div style="display: flex; flex-direction: column; align-items: center; gap: 20px; padding-top: 18px">{pod(k, a, s, hk)}{cap(t)}</div>'
                   for k, a, s, hk, t in [("neutrophil", 142, "idle", 1, "Ready"), ("neutrophil", 142, "hover", 1, "Hover"), ("cytotoxic_t", 142, "armed", 2, "Armed"),
                                          ("macrophage", 142, "idle", 3, "Short on ATP"), ("goblet_cell", 142, "locked", 4, "Not in level")])
    ves = "".join(f'<div style="display: flex; flex-direction: column; align-items: center; gap: 8px; padding-top: 12px">{vesicle(ab, s, r)}{cap(t)}</div>'
                  for ab, s, r, t in [(ABILITIES[1], "ready", 0, "Ready"), (ABILITIES[0], "cooling", 62, "Cooling"), (ABILITIES[1], "armed", 0, "Aiming")])
    icons = "".join(f'<div style="display: flex; flex-direction: column; align-items: center; gap: 4px">{ic}{cap(t)}</div>'
                    for ic, t in [(mito(56), "ATP"), (memory_icon(48), "Memory cell"), (antibody_icon(48), "Antibody")])
    type_ = (f'<div style="display: flex; flex-direction: column; gap: 8px">'
             f'<div style="display: flex; align-items: baseline; gap: 12px"><span style="font-family: {DISPLAY}; font-weight: 600; font-size: 46px; line-height: 1">142</span><span style="font-size: 14px; font-weight: 700; color: {INK3}">Fredoka 600</span></div>'
             f'<div style="display: flex; align-items: baseline; gap: 12px"><span style="font-size: 17px; font-weight: 800">Cytotoxic T</span><span style="font-size: 14px; font-weight: 700; color: {INK3}">Nunito 700–900</span></div>'
             f'<div style="display: flex; align-items: baseline; gap: 12px">{lab("Organ integrity", INK2)}<span style="font-size: 14px; font-weight: 700; color: {INK3}">Label, 13 px minimum</span></div></div>')
    body = (f'<div style="position: absolute; left: 64px; top: 48px"><span style="font-family: {DISPLAY}; font-weight: 700; font-size: 54px; color: #fff">IMMUNE HUD kit</span></div>'
            + kit_block(48, 140, 600, 470, "Colour", f'<div style="display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px 18px">{swatches}</div>', "host", 1)
            + kit_block(48, 640, 600, 250, "Pathogens", f'<div style="display: flex; justify-content: space-around">{germs}</div>', "host", 2)
            + kit_block(48, 920, 600, 270, "Type", type_, "host", 3)
            + kit_block(690, 140, 800, 330, "Towers", f'<div style="display: flex; justify-content: space-between">{towers}</div>', "immune", 4)
            + kit_block(690, 500, 800, 330, "Build pods", f'<div style="display: flex; gap: 12px">{pods}</div>', "immune", 5)
            + kit_block(690, 860, 800, 330, "Abilities", f'<div style="display: flex; gap: 40px">{ves}</div>', "immune", 6)
            + kit_block(1530, 140, 344, 300, "Icons", f'<div style="display: flex; justify-content: space-between">{icons}</div>', "host", 7))
    return page("IMMUNE HUD kit", body, kw, kh, MATRIX)


if __name__ == "__main__":
    BOARDS = [("Main.dc.html", scene_main), ("Placing.dc.html", scene_place), ("Inspect.dc.html", scene_inspect),
              ("Prep.dc.html", scene_prep), ("Critical.dc.html", scene_critical), ("Kit.dc.html", scene_kit)]
    for name, fn in BOARDS:
        html = fn()
        with open(os.path.join(PROJ, name), "w", encoding="utf-8") as f:
            f.write(html)
        print(name, len(html))
    for k, p in TW:
        print(k, p, round(lane_dist(p)))
