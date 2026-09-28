"""Extracts the UI icons from the design canvas snapshot into assets/ui/icons/.

The design canvas (docs/ui-concepts/canvas/project/*.dc.html, a snapshot of
https://claude.ai/artifact/BmwSmwUEERL8X7rbVyfvWp) draws every icon as a small
inline SVG. This script pulls them out verbatim, so the game ships exactly the
drawings that were designed, and re-running it after the canvas changes
refreshes them. Output is plain SVG text, one file per icon, loaded at runtime
by src/gui/icons/IconLibrary (nanosvg).

Every icon is normalized to a 64x64 viewBox with the Kit's root convention
(fill="none", round caps and joins): the canvas relies on that for its
stroke-only paths.

Usage:  python tools/extract_icons.py [--check]
  --check  fail (exit 1) if the committed icons differ from a fresh extraction
"""

import argparse
import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CANVAS = ROOT / "docs" / "ui-concepts" / "canvas" / "project"
OUT = ROOT / "assets" / "ui" / "icons"

HEADER = ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="{vb}" '
          'fill="none" stroke-linecap="round" stroke-linejoin="round">\n')

# Named glyphs in the Strengthen Immunity top bar (Tree.dc.html GLYPHS list).
TREE_NAMES = {
    "a_cascade": "ability_complement_cascade",
    "a_clot": "ability_fibrin_clot",
    "a_fever": "ability_fever_response",
    "a_histamine": "ability_histamine_flare",
    "t_neutrophil": "tower_neutrophil",
    "t_cytotoxic_t": "tower_cytotoxic_t",
    "t_macrophage": "tower_macrophage",
    "t_goblet_cell": "tower_goblet_cell",
    "t_fibroblast": "tower_fibroblast",
}

# Icons identified by the label printed right after them in the HUD and Kit.
LABELLED = [
    (re.compile(r"^Bacteria\b"), "pathogen_bacteria"),
    (re.compile(r"^Virus(es)?\b"), "pathogen_virus"),
    (re.compile(r"^Parasites?\b"), "pathogen_parasite"),
    (re.compile(r"^Elite\b"), "marker_elite"),
    (re.compile(r"^Memory cells?\b"), "memory_cell"),
    (re.compile(r"^Antibod(y|ies)\b"), "antibody"),
    (re.compile(r"^\d+ ATP\b"), "atp"),
]

# Glyphs on round control buttons, identified by the button's aria-label.
BUTTON_GLYPHS = {
    "Pause": "glyph_pause",
    "Menu": "glyph_menu",
    "Back to Strengthen Immunity": "glyph_back",
}

SVG_RE = re.compile(r'<svg\b([^>]*)>(.*?)</svg>', re.S)


def text_after(s: str, pos: int, n: int = 400) -> str:
    t = re.sub(r"<[^>]+>", " ", s[pos:pos + n])
    return re.sub(r"\s+", " ", html.unescape(t)).strip()


def strip_outer_group(body: str) -> tuple[str, str]:
    """Returns (transform, inner) for a body that is one <g transform=...>."""
    m = re.match(r'\s*<g transform="([^"]+)">(.*)</g>\s*$', body, re.S)
    return (m.group(1), m.group(2)) if m else ("", body)


def extract() -> dict[str, str]:
    icons: dict[str, str] = {}

    tree = (CANVAS / "Tree.dc.html").read_text(encoding="utf-8")
    for m in re.finditer(r'<svg style="display: \{\{d\.show\.([a-z_]+)\}\}[^>]*>(.*?)</svg>', tree, re.S):
        key, body = m.group(1), m.group(2)
        transform, inner = strip_outer_group(body)
        name = TREE_NAMES.get(key, f"stat_{key}")
        if "translate(-32 -32)" in transform:
            content = inner  # already in 64-unit space
        else:
            content = f'<g transform="translate(32 32)">{inner}</g>'  # origin-centred glyph
        icons[name] = HEADER.format(vb="0 0 64 64") + content + "\n</svg>\n"

    for path in sorted(CANVAS.glob("*.dc.html")):
        s = path.read_text(encoding="utf-8")
        for m in SVG_RE.finditer(s):
            attrs, body = m.group(1), m.group(2)
            vb = re.search(r'viewBox="([^"]+)"', attrs)
            if not vb or vb.group(1) not in ("0 0 64 64", "0 0 24 24"):
                continue
            label = text_after(s, m.end())
            for pattern, name in LABELLED:
                if name not in icons and pattern.search(label):
                    icons[name] = HEADER.format(vb=vb.group(1)) + body.strip() + "\n</svg>\n"
            if vb.group(1) == "0 0 24 24" and label.startswith("Play") and "glyph_play" not in icons:
                icons["glyph_play"] = HEADER.format(vb="0 0 24 24") + body.strip() + "\n</svg>\n"
            aria = re.findall(r'aria-label="([^"]+)"', s[max(0, m.start() - 600):m.start()])
            if aria and aria[-1] in BUTTON_GLYPHS and vb.group(1) == "0 0 64 64":
                name = BUTTON_GLYPHS[aria[-1]]
                if name in icons:
                    continue
                # Drop the button's own membrane (long blob paths and its
                # scaled outline): what is left is the glyph.
                glyph = re.sub(r'<path d="M[^"]{300,}"[^>]*/>', "", body)
                glyph = re.sub(r"<defs>.*?</defs>", "", glyph, flags=re.S)
                if re.search(r"<(path|rect|circle|ellipse|g)\b", glyph):
                    icons[name] = HEADER.format(vb="0 0 64 64") + glyph.strip() + "\n</svg>\n"
    return icons


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    icons = extract()
    if args.check:
        stale = [n for n, svg in icons.items()
                 if not (OUT / f"{n}.svg").exists() or (OUT / f"{n}.svg").read_text(encoding="utf-8") != svg]
        for n in stale:
            print(f"stale: {n}.svg")
        return 1 if stale else 0
    OUT.mkdir(parents=True, exist_ok=True)
    for name, svg in sorted(icons.items()):
        (OUT / f"{name}.svg").write_text(svg, encoding="utf-8")
        print(f"{name}.svg  {len(svg)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
