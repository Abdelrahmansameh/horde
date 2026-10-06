"""Generates the Strengthen Immunity tree's layout: a human figure.

The tree (src/game/meta/ImmunityTree.cpp) is laid out so that its nodes and
the vessels between them draw the body they strengthen -- no outline, the
pattern alone. The Neutrophil, owned from the start, is the heart, ringed by
its own lines like ribs; the hub lines run up the sternum and the neck and
down the middle of the abdomen to the pelvis; two abilities ring the head and
two run down the sides of the waist; each of the other four towers is a limb,
from its unlock at the shoulder or hip to its capstone at the hand or foot.
This script places every node and writes the placement to
assets/ui/tree_layout.json, which the game's tree screen
(src/ui/front/TreeScreen) draws from.

The tree's SHAPE (who is whose parent) is the game's: it is read from
ImmunityTree.cpp's kEdges table, and the vessels between nodes are drawn from
it at run time. This script only decides where each node sits. It checks
that every node of the catalog is placed, that the Neutrophil is the only
root, that no node has more than three children, and that no two nodes
overlap.

Output, in tree units (one unit is one logical pixel at zoom 1), y down, the
heart at the origin:
  nodes  game key -> centre, diameter, and the glyph icon it shows

Usage:  python tools/gen_tree_layout.py [--check] [--preview out.png]
  --check    fail (exit 1) if the committed layout differs from a fresh one
  --preview  also draw the fully grown tree the way the game does, as a PNG
             (needs Pillow)
"""

import argparse
import json
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "game" / "meta" / "ImmunityTree.h"
SOURCE = ROOT / "src" / "game" / "meta" / "ImmunityTree.cpp"
OUT = ROOT / "assets" / "ui" / "tree_layout.json"

# ---------------------------------------------------------------- the catalog


def read_catalog():
    """(keys in TreeNode order, {key: kind}, {child key: parent key})"""
    header = HEADER.read_text(encoding="utf-8")
    block = header[header.index("enum class TreeNode : u16 {"):]
    block = block[:block.index("Count,")]
    enum = re.findall(r"^\s*([A-Z]\w*)(?:\s*=\s*0)?,", block, re.M)
    source = SOURCE.read_text(encoding="utf-8")
    nodes = re.findall(r'node\("([a-z_.]+)", "[^"]*", "[^"]*", K::(\w+),', source)
    if len(enum) != len(nodes):
        sys.exit(f"TreeNode has {len(enum)} entries but kCatalog has {len(nodes)}")
    key_of = {name: key for name, (key, _) in zip(enum, nodes)}
    edges = re.findall(r"\{N::(\w+), N::(\w+)\}", source)
    parent = {key_of[c]: key_of[p] for c, p in edges}
    return [k for k, _ in nodes], dict(nodes), parent


KEYS, KIND, PARENT = read_catalog()

# Diameter by kind, in tree units.
SIZE = {"TowerRoot": 108, "AbilityRoot": 84, "Capstone": 92, "Stat": 62, "Economy": 62, "AbilityStat": 60}

# The glyph each node shows (assets/ui/icons). Tower and ability unlocks show
# their cell or ability; every other node, what kind of number it raises.
ICON = {
    "hub.bone_marrow_reserve": "stat_atp",
    "hub.rapid_metabolism": "stat_atp",
    "hub.efficient_clearance": "stat_atp",
    "hub.field_requisition": "stat_atp",
    "hub.systemic_potency": "stat_damage",
    "hub.elite_response": "stat_damage",
    "hub.homeostasis": "stat_health",
    "hub.membrane_resilience": "stat_health",
    "ability.complement.unlock": "ability_complement_cascade",
    "ability.complement.cooldown": "stat_cadence",
    "ability.complement.chain": "stat_count",
    "ability.histamine.unlock": "ability_histamine_flare",
    "ability.histamine.cooldown": "stat_cadence",
    "ability.histamine.radius": "stat_reach",
    "ability.fever.unlock": "ability_fever_response",
    "ability.fever.cooldown": "stat_cadence",
    "ability.fever.magnitude": "stat_health",
    "ability.clot.unlock": "ability_fibrin_clot",
    "ability.clot.cooldown": "stat_cadence",
    "ability.clot.duration": "stat_time",
    "neutrophil.unlock": "tower_neutrophil",
    "neutrophil.round_damage": "stat_damage",
    "neutrophil.trigger_rate": "stat_cadence",
    "neutrophil.aggro_range": "stat_reach",
    "neutrophil.squad_size": "stat_count",
    "neutrophil.accuracy": "stat_aim",
    "neutrophil.vitality": "stat_health",
    "neutrophil.capstone": "stat_fire",
    "cytotoxic.unlock": "tower_cytotoxic_t",
    "cytotoxic.drain": "stat_damage",
    "cytotoxic.attach_speed": "stat_cadence",
    "cytotoxic.search": "stat_reach",
    "cytotoxic.stamina": "stat_time",
    "cytotoxic.tower_health": "stat_health",
    "cytotoxic.capstone": "stat_damage",
    "macrophage.unlock": "tower_macrophage",
    "macrophage.arms": "stat_count",
    "macrophage.grab_speed": "stat_cadence",
    "macrophage.captives": "stat_count",
    "macrophage.search": "stat_reach",
    "macrophage.health": "stat_health",
    "macrophage.wall": "stat_wall",
    "macrophage.capstone": "stat_health",
    "goblet.unlock": "tower_goblet_cell",
    "goblet.slow_strength": "stat_time",
    "goblet.splash_radius": "stat_reach",
    "goblet.slow_duration": "stat_time",
    "goblet.weakness": "stat_fire",
    "goblet.tower_health": "stat_health",
    "goblet.capstone": "stat_time",
    "fibroblast.unlock": "tower_fibroblast",
    "fibroblast.scar_health": "stat_health",
    "fibroblast.reinforce": "stat_cadence",
    "fibroblast.scar_size": "stat_wall",
    "fibroblast.build_radius": "stat_reach",
    "fibroblast.inflammation": "stat_fire",
    "fibroblast.tower_health": "stat_health",
    "fibroblast.capstone": "stat_fire",
}

# ---------------------------------------------------------------- 2D helpers


def add(a, b):
    return (a[0] + b[0], a[1] + b[1])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def mul(a, k):
    return (a[0] * k, a[1] * k)


def norm(a):
    length = math.hypot(a[0], a[1]) or 1.0
    return (a[0] / length, a[1] / length)


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def mirror(p):
    return (-p[0], p[1])


# ---------------------------------------------------------------- the figure
# Tree units, y down, the heart at the origin. The limbs' joints are the right
# half's (screen right); the left half mirrors them.

SHOULDER, ELBOW, WRIST, HAND = (330, -262), (690, -168), (1000, -88), (1082, -64)
HIP, KNEE, ANKLE, FOOT = (172, 600), (210, 1040), (232, 1420), (268, 1474)
HEAD, HEAD_RADIUS = (0, -610), 150

POS = {}


def put(key, p):
    if key in POS:
        sys.exit(f"{key} placed twice")
    POS[key] = (round(p[0], 1), round(p[1], 1))


def pair(left, right, p):
    """A left/right pair at mirrored places; `p` is the right one."""
    put(left, mirror(p))
    put(right, p)


def place_nodes():
    # The spine: sternum, heart, solar plexus, stomach, navel, pelvis; and
    # the neck up to the head.
    put("hub.bone_marrow_reserve", (0, -250))
    put("neutrophil.unlock", (0, -78))
    put("neutrophil.round_damage", (0, 92))
    put("hub.rapid_metabolism", (0, 252))
    put("hub.efficient_clearance", (0, 404))
    put("hub.membrane_resilience", (0, 552))
    put("hub.elite_response", (0, -352))

    # The head: seven nodes evenly round a ring, Homeostasis at the bottom
    # where the neck joins, Histamine Flare up the left side and Fever
    # Response up the right, their last lines meeting at the crown.
    ring = ["hub.homeostasis", "ability.histamine.unlock", "ability.histamine.cooldown",
            "ability.histamine.radius", "ability.fever.magnitude", "ability.fever.cooldown",
            "ability.fever.unlock"]
    for k, key in enumerate(ring):
        a = math.radians(180.0 + k * 360.0 / len(ring))   # clockwise from the top
        put(key, add(HEAD, (HEAD_RADIUS * math.sin(a), -HEAD_RADIUS * math.cos(a))))

    # The ribs curl up around the heart, the capstone on the left.
    pair("neutrophil.trigger_rate", "neutrophil.accuracy", (178, 42))
    pair("neutrophil.squad_size", "neutrophil.aggro_range", (218, -104))
    pair("neutrophil.capstone", "neutrophil.vitality", (152, -230))

    # The belly: Complement Cascade down the left side of the waist and
    # Fibrin Clot down the right, the economy down the middle.
    pair("ability.complement.unlock", "ability.clot.unlock", (196, 228))
    pair("ability.complement.cooldown", "ability.clot.cooldown", (212, 362))
    pair("ability.complement.chain", "ability.clot.duration", (200, 492))
    pair("hub.field_requisition", "hub.systemic_potency", (100, 470))

    # The arms: the unlock at the shoulder, a chain down the arm to the
    # capstone in the hand, and one bud on the underside of the upper arm.
    def arm(side, root, a, bud, b, c, d, cap):
        f = (lambda q: q) if side > 0 else mirror
        up = norm(sub(ELBOW, SHOULDER))
        under = (-up[1], up[0])          # towards the body, below the arm
        put(root, f((352, -268)))
        put(a, f(lerp(SHOULDER, ELBOW, 0.40)))
        put(bud, f(add(lerp(SHOULDER, ELBOW, 0.66), mul(under, 50))))
        put(b, f(add(lerp(SHOULDER, ELBOW, 0.92), mul(under, -12))))
        put(c, f(lerp(ELBOW, WRIST, 0.42)))
        put(d, f(lerp(ELBOW, WRIST, 0.88)))
        put(cap, f(add(HAND, (14, 4))))

    arm(-1, "cytotoxic.unlock", "cytotoxic.drain", "cytotoxic.tower_health", "cytotoxic.attach_speed",
        "cytotoxic.search", "cytotoxic.stamina", "cytotoxic.capstone")
    arm(1, "goblet.unlock", "goblet.slow_strength", "goblet.tower_health", "goblet.slow_duration",
        "goblet.splash_radius", "goblet.weakness", "goblet.capstone")

    # The legs: the unlock at the hip, a chain down to the capstone at the
    # foot, a bud on each side of the thigh.
    def leg(side, root, a, bud_out, b, bud_in, c, d, cap):
        f = (lambda q: q) if side > 0 else mirror
        down = norm(sub(KNEE, HIP))
        outward = (down[1], -down[0])
        put(root, f((190, 690)))
        put(a, f(lerp(HIP, KNEE, 0.48)))
        put(bud_out, f(add(lerp(HIP, KNEE, 0.62), mul(outward, 64))))
        put(b, f(lerp(HIP, KNEE, 0.86)))
        put(bud_in, f(add(lerp(HIP, KNEE, 1.04), mul(outward, -66))))
        put(c, f(lerp(KNEE, ANKLE, 0.34)))
        put(d, f(lerp(KNEE, ANKLE, 0.74)))
        put(cap, f(add(FOOT, (6, 0))))

    leg(-1, "macrophage.unlock", "macrophage.grab_speed", "macrophage.arms", "macrophage.captives",
        "macrophage.health", "macrophage.search", "macrophage.wall", "macrophage.capstone")
    leg(1, "fibroblast.unlock", "fibroblast.scar_health", "fibroblast.tower_health", "fibroblast.reinforce",
        "fibroblast.build_radius", "fibroblast.scar_size", "fibroblast.inflammation", "fibroblast.capstone")


def check_nodes():
    missing = [k for k in KEYS if k not in POS]
    extra = [k for k in POS if k not in KIND]
    no_icon = [k for k in KEYS if k not in ICON]
    if missing or extra or no_icon:
        sys.exit(f"unplaced: {missing}  unknown: {extra}  no icon: {no_icon}")
    roots = [k for k in KEYS if k not in PARENT]
    if roots != ["neutrophil.unlock"]:
        sys.exit(f"the tree must have the Neutrophil as its only root, not {roots}")
    children = {}
    for child, parent in PARENT.items():
        children.setdefault(parent, []).append(child)
    for parent, kids in children.items():
        if len(kids) > 3:
            sys.exit(f"{parent} has {len(kids)} children: {kids}")
    # No two nodes closer than a third of a small node between their rims.
    keys = sorted(POS)
    for i, a in enumerate(keys):
        for b in keys[i + 1:]:
            gap = math.dist(POS[a], POS[b]) - (SIZE[KIND[a]] + SIZE[KIND[b]]) / 2
            if gap < 20:
                sys.exit(f"{a} and {b} overlap (gap {gap:.0f})")


# ---------------------------------------------------------------- output


def build():
    place_nodes()
    check_nodes()
    nodes = {k: {"x": POS[k][0], "y": POS[k][1], "size": SIZE[KIND[k]], "icon": ICON[k]} for k in KEYS}
    return {
        "_comment": "Generated by tools/gen_tree_layout.py: the Strengthen Immunity tree laid out as a human "
                    "figure, in tree units (one logical pixel at zoom 1), y down, the heart at the origin. "
                    "Do not edit by hand.",
        "nodes": nodes,
    }


def vessel(pos, child):
    """The game's vessel from a node's parent to it (TreeCanvas::add_edge): a
    cubic leaving the parent partly along the vessel that feeds it."""
    parent = PARENT[child]
    a, b = pos[parent], pos[child]
    length = math.dist(a, b)
    d = norm(sub(b, a))
    trunk = norm(sub(a, pos[PARENT[parent]])) if parent in PARENT else d
    leave = norm(add(mul(trunk, 0.45), d))
    c1, c2 = add(a, mul(leave, length * 0.38)), sub(b, mul(d, length * 0.30))
    points = []
    for i in range(25):
        t = i / 24
        u = 1 - t
        points.append(add(add(mul(a, u * u * u), mul(c1, 3 * u * u * t)), add(mul(c2, 3 * u * t * t), mul(b, t * t * t))))
    return points


def preview(layout, path, scale=0.42, supersample=2):
    """The fully grown tree as the game draws it: vessels and nodes."""
    from PIL import Image, ImageDraw

    pos = {k: (n["x"], n["y"]) for k, n in layout["nodes"].items()}
    xs = [p[0] for p in pos.values()]
    ys = [p[1] for p in pos.values()]
    x0, y0 = min(xs) - 90, min(ys) - 90
    s = scale * supersample
    w, h = int((max(xs) + 90 - x0) * s), int((max(ys) + 90 - y0) * s)

    def t(p):
        return ((p[0] - x0) * s, (p[1] - y0) * s)

    img = Image.new("RGB", (w, h), (74, 18, 40))
    dr = ImageDraw.Draw(img)
    curves = [[t(p) for p in vessel(pos, child)] for child in PARENT]
    for width, colour in ((14, (78, 22, 56)), (7, (164, 140, 240))):
        for c in curves:
            dr.line(c, fill=colour, width=max(1, int(width * s)), joint="curve")
    colour = {"TowerRoot": (164, 140, 240), "AbilityRoot": (255, 214, 120), "Capstone": (242, 178, 51)}
    for k, n in layout["nodes"].items():
        r = n["size"] / 2 * s
        x, y = t(pos[k])
        dr.ellipse([x - r, y - r, x + r, y + r], fill=colour.get(KIND[k], (247, 243, 255)),
                   outline=(78, 22, 56), width=max(1, int(5 * s)))
    img.resize((w // supersample, h // supersample), Image.LANCZOS).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--preview")
    args = ap.parse_args()
    layout = build()
    text = json.dumps(layout, indent=1) + "\n"
    if args.preview:
        preview(layout, args.preview)
    if args.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            print(f"stale: {OUT.relative_to(ROOT)}")
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8")
    print(f"{OUT.relative_to(ROOT)}: {len(layout['nodes'])} nodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
