"""Builds the static UI fonts in assets/fonts/ from Google Fonts' variable fonts.

The UI (src/gui/text) rasterizes with stb_truetype, which reads only a
variable font's default instance, so each weight the design uses is cut out
as its own static .ttf here and subset to the Latin range the game needs.

Usage (needs fontTools: pip install fonttools):
    python tools/build_fonts.py            # download sources, write assets/fonts/
    python tools/build_fonts.py --src DIR  # use Fredoka-VF.ttf / Nunito-VF.ttf from DIR

Weights come from the design canvas (docs/ui-concepts/canvas): Fredoka
500/600/700 for numbers and titles, Nunito 600/700/800/900 for labels.
"""

import argparse
import pathlib
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "fonts"
BASE = "https://raw.githubusercontent.com/google/fonts/main/ofl"

SOURCES = {
    "Fredoka": ("fredoka/Fredoka%5Bwdth,wght%5D.ttf", "fredoka/OFL.txt"),
    "Nunito": ("nunito/Nunito%5Bwght%5D.ttf", "nunito/OFL.txt"),
}

WEIGHTS = {
    "Fredoka": {500: "Medium", 600: "SemiBold", 700: "Bold"},
    "Nunito": {600: "SemiBold", 700: "Bold", 800: "ExtraBold", 900: "Black"},
}

# Basic Latin, Latin-1, and the punctuation/arrows/math the UI prints.
UNICODES = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x100))
    + [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
       0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x221E]
)


def fetch(rel: str, dest: pathlib.Path) -> None:
    with urllib.request.urlopen(f"{BASE}/{rel}") as r:
        dest.write_bytes(r.read())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=pathlib.Path, help="directory holding <Family>-VF.ttf")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    src = args.src or (OUT / ".src")
    src.mkdir(parents=True, exist_ok=True)

    licences = []
    for family, (font_rel, ofl_rel) in SOURCES.items():
        vf = src / f"{family}-VF.ttf"
        if not vf.exists():
            fetch(font_rel, vf)
        ofl = src / f"OFL-{family}.txt"
        if not ofl.exists():
            fetch(ofl_rel, ofl)
        licences.append(f"==== {family} ====\n\n{ofl.read_text(encoding='utf-8')}")

        for weight, style in WEIGHTS[family].items():
            font = TTFont(vf)
            axes = {"wght": weight}
            if any(a.axisTag == "wdth" for a in font["fvar"].axes):
                axes["wdth"] = 100
            static = instancer.instantiateVariableFont(font, axes, updateFontNames=True)
            opts = subset.Options()
            opts.layout_features = ["kern", "liga", "tnum", "lnum"]
            opts.name_IDs = ["*"]
            sub = subset.Subsetter(opts)
            sub.populate(unicodes=UNICODES)
            sub.subset(static)
            path = OUT / f"{family}-{style}.ttf"
            static.save(path)
            print(f"{path.relative_to(ROOT)}  {path.stat().st_size // 1024} KB")

    (OUT / "OFL.txt").write_text("\n\n".join(licences), encoding="utf-8")
    if not args.src:
        for f in src.iterdir():
            f.unlink()
        src.rmdir()


if __name__ == "__main__":
    main()
