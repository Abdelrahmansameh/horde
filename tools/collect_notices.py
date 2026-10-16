"""Collect required dependency and font licenses for the standalone executable."""

import argparse
from pathlib import Path


DEPENDENCIES = (
    "sdl2", "glad", "glm", "entt", "nlohmann-json", "imgui", "stb", "nanosvg",
)
OPTIONAL_DEPENDENCIES = ("opengl-registry", "egl-registry")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcpkg-root", required=True, type=Path,
                        help="Installed target triplet directory")
    parser.add_argument("--fonts", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    sections = ["IMMUNE — Third-party notices\n\n"
                "This executable contains the following third-party software and fonts.\n"]
    for name in DEPENDENCIES + OPTIONAL_DEPENDENCIES:
        path = args.vcpkg_root / "share" / name / "copyright"
        if not path.is_file() and name in OPTIONAL_DEPENDENCIES:
            continue
        if not path.is_file():
            parser.error(f"Required dependency copyright file is missing: {path}")
        sections.append(f"\n{'=' * 72}\n{name}\n{'=' * 72}\n\n"
                        + path.read_text(encoding="utf-8-sig").rstrip() + "\n")
    sections.append(f"\n{'=' * 72}\nFonts\n{'=' * 72}\n\n"
                    + args.fonts.read_text(encoding="utf-8-sig").rstrip() + "\n")
    text = "".join(sections)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
