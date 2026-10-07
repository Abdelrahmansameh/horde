"""Generates the Strengthen Immunity tree's layout: a classic radial tree.

The tree (src/game/meta/ImmunityTree.cpp) is laid out radially from its
root. The Neutrophil, owned from the start, is the centre; the three core
economy lines sit round it; and from each core its subjects grow outward, each
in its own wedge -- the attack towers (Macrophage, Neutrophil, Cytotoxic T),
the abilities, and the systemic lines with the control towers (Fibroblast,
Goblet Cell). This script places every node and writes the placement to
assets/ui/tree_layout.json, which the game's tree screen
(src/ui/front/TreeScreen) draws from.

The tree's SHAPE (who is whose parent, and the clockwise order of a node's
children) is the game's: it is read from ImmunityTree.cpp's kEdges table, and
the vessels between nodes are drawn from it at run time. This script only
works out where each node sits:

  - a node's depth sets its ring (RING_1 out from the centre for the cores,
    RING_STEP further for each level after that);
  - every node gets a wedge of its parent's, in proportion to how many leaves
    hang below it, and sits in the middle of it; the subtrees under the
    centre and the cores keep PETAL_GAP between them, so each subject reads as
    its own region;
  - the whole tree is turned so the Neutrophil's own lines grow straight up.

It checks that every node of the catalog is placed, that the Neutrophil is
the only root, that no node has more than three children, and that no two
nodes overlap.

Output, in tree units (one unit is one logical pixel at zoom 1), y down, the
centre at the origin:
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

# Tree units.
RING_1 = 180.0
RING_STEP = 165.0
PETAL_GAP = math.radians(9.0)
# The node whose wedge points straight up.
UP = "neutrophil.round_damage"

# ---------------------------------------------------------------- the catalog


def read_catalog():
    """(keys in TreeNode order, {key: kind}, [(child, parent)] in kEdges order)"""
    header = HEADER.read_text(encoding="utf-8")
    block = header[header.index("enum class TreeNode : u16 {"):]
    block = block[:block.index("Count,")]
    enum = re.findall(r"^\s*([A-Z]\w*)(?:\s*=\s*0)?,", block, re.M)
    source = SOURCE.read_text(encoding="utf-8")
    nodes = re.findall(r'node\("([a-z_.]+)", "[^"]*", "[^"]*", K::(\w+),', source)
    if len(enum) != len(nodes):
        sys.exit(f"TreeNode has {len(enum)} entries but kCatalog has {len(nodes)}")
    key_of = {name: key for name, (key, _) in zip(enum, nodes)}
    edges = [(key_of[c], key_of[p]) for c, p in re.findall(r"\{N::(\w+), N::(\w+)\}", source)]
    return [k for k, _ in nodes], dict(nodes), edges


KEYS, KIND, EDGES = read_catalog()
PARENT = dict(EDGES)
CHILDREN = {}
for _child, _parent in EDGES:
    CHILDREN.setdefault(_parent, []).append(_child)

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

# ---------------------------------------------------------------- the layout


def root():
    roots = [k for k in KEYS if k not in PARENT]
    if roots != ["neutrophil.unlock"]:
        sys.exit(f"the tree must have the Neutrophil as its only root, not {roots}")
    return roots[0]


def leaves(key):
    kids = CHILDREN.get(key, [])
    return 1 if not kids else sum(leaves(k) for k in kids)


def place_nodes():
    """{key: (x, y)}: the radial layout described at the top of the file."""
    angle, depth = {}, {}

    def grow(key, a0, a1, d):
        angle[key], depth[key] = (a0 + a1) / 2, d
        kids = CHILDREN.get(key, [])
        if not kids:
            return
        gap = PETAL_GAP if d <= 1 else 0.0
        usable = (a1 - a0) - gap * len(kids)
        total = sum(leaves(k) for k in kids)
        a = a0
        for k in kids:
            span = usable * leaves(k) / total
            grow(k, a + gap / 2, a + gap / 2 + span, d + 1)
            a += span + gap

    centre = root()
    grow(centre, 0.0, 2.0 * math.pi, 0)
    turn = -math.pi / 2 - angle[UP]          # screen y is down: -90 degrees is up
    pos = {}
    for key in angle:
        r = 0.0 if depth[key] == 0 else RING_1 + (depth[key] - 1) * RING_STEP
        a = angle[key] + turn
        pos[key] = (round(r * math.cos(a), 1), round(r * math.sin(a), 1))
    return pos


def check_nodes(pos):
    missing = [k for k in KEYS if k not in pos]
    no_icon = [k for k in KEYS if k not in ICON]
    if missing or no_icon:
        sys.exit(f"unplaced: {missing}  no icon: {no_icon}")
    for parent, kids in CHILDREN.items():
        if len(kids) > 3:
            sys.exit(f"{parent} has {len(kids)} children: {kids}")
    # No two nodes closer than a third of a small node between their rims.
    keys = sorted(pos)
    for i, a in enumerate(keys):
        for b in keys[i + 1:]:
            gap = math.dist(pos[a], pos[b]) - (SIZE[KIND[a]] + SIZE[KIND[b]]) / 2
            if gap < 20:
                sys.exit(f"{a} and {b} overlap (gap {gap:.0f})")


# ---------------------------------------------------------------- output


def build():
    pos = place_nodes()
    check_nodes(pos)
    nodes = {k: {"x": pos[k][0], "y": pos[k][1], "size": SIZE[KIND[k]], "icon": ICON[k]} for k in KEYS}
    return {
        "_comment": "Generated by tools/gen_tree_layout.py: the Strengthen Immunity tree laid out radially, "
                    "each subject in its own wedge, in tree units (one logical pixel at zoom 1), y down, "
                    "the centre at the origin. Do not edit by hand.",
        "nodes": nodes,
    }


def add(a, b):
    return (a[0] + b[0], a[1] + b[1])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def mul(a, k):
    return (a[0] * k, a[1] * k)


def norm(a):
    length = math.hypot(a[0], a[1]) or 1.0
    return (a[0] / length, a[1] / length)


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


def preview(layout, path, scale=0.5, supersample=2):
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
