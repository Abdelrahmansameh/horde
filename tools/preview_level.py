#!/usr/bin/env python3
"""tools/preview_level.py — renders an .imff flow-field dump to a PNG.

Owner: Wave 1A (src/sim/flowfield). The dump format is written by
src/sim/flowfield/FlowFieldDebug.h::dump_flow_field and documented there:

    char[4]  magic  = "IMFF"
    u32      version = 1
    i32      width, height
    f32      cell_size
    f32      origin_x, origin_y
    u8   [w*h]  walkable
    f32  [w*h]  mask cost multiplier
    f32  [w*h]  signed distance   (0 if no DistanceField was supplied)
    f32  [w*h]  cost-to-goal      (non-finite written as -1.0)
    f32  [w*h*2] direction x,y

Deliberately stdlib-only (struct + zlib for a minimal PNG encoder). The project
ships zero binary assets and agents cannot rely on `pip install` succeeding
(or being permitted) in a sandboxed build environment, so this tool has no
third-party dependency — not even Pillow/matplotlib.

Usage:
    python tools/preview_level.py field.imff -o field.png
    python tools/preview_level.py field.imff -o field.png --mode cost
    python tools/preview_level.py field.imff -o field.png --mode mask --no-arrows
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"IMFF"


class FlowFieldDump:
    def __init__(self, path: Path):
        data = path.read_bytes()
        if data[:4] != MAGIC:
            raise ValueError(f"{path}: bad magic {data[:4]!r}, expected {MAGIC!r}")
        off = 4
        (version,) = struct.unpack_from("<I", data, off); off += 4
        if version != 1:
            raise ValueError(f"{path}: unsupported version {version}")
        w, h = struct.unpack_from("<ii", data, off); off += 8
        cell_size, ox, oy = struct.unpack_from("<fff", data, off); off += 12

        n = w * h
        self.width = w
        self.height = h
        self.cell_size = cell_size
        self.origin = (ox, oy)

        self.walkable = struct.unpack_from(f"<{n}B", data, off); off += n
        self.mask_cost = struct.unpack_from(f"<{n}f", data, off); off += n * 4
        self.sdf = struct.unpack_from(f"<{n}f", data, off); off += n * 4
        self.cost = struct.unpack_from(f"<{n}f", data, off); off += n * 4
        dirs = struct.unpack_from(f"<{2 * n}f", data, off); off += n * 8
        self.dir_x = dirs[0::2]
        self.dir_y = dirs[1::2]

        expected = off
        if len(data) != expected:
            raise ValueError(f"{path}: size mismatch, read {expected} bytes, file is {len(data)}")

    def at(self, x: int, y: int) -> int:
        return y * self.width + x


# --------------------------------------------------------------------------
# Minimal PNG writer (RGB8, no external dependency)
# --------------------------------------------------------------------------

def write_png(path: Path, rgb_rows: list[bytes], width: int, height: int) -> None:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)  # filter type: none
        raw.extend(row)
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

WALL_COLOR = (24, 22, 30)
TISSUE_COLOR = (168, 84, 100)          # warm tissue-substrate, matches Wave 0 clear color
UNREACHABLE_COLOR = (120, 40, 160)     # dead-end pockets: clearly distinct purple
GOAL_TINT = (255, 240, 120)
ARROW_COLOR = (250, 250, 250)
COST_LOW = (60, 140, 255)              # near goal: blue
COST_HIGH = (255, 210, 60)             # far from goal: amber


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def lerp_color(c0, c1, t: float):
    return tuple(int(round(lerp(c0[i], c1[i], max(0.0, min(1.0, t))))) for i in range(3))


def cell_color(dump: FlowFieldDump, x: int, y: int, mode: str, max_finite_cost: float):
    i = dump.at(x, y)
    if not dump.walkable[i]:
        return WALL_COLOR
    c = dump.cost[i]
    if mode == "mask":
        # Cost multiplier heatmap: 1.0 -> tissue color, higher -> darker/redder.
        mul = dump.mask_cost[i]
        t = max(0.0, min(1.0, (mul - 1.0) / 4.0))
        return lerp_color(TISSUE_COLOR, (90, 20, 20), t)
    if c < 0.0:
        return UNREACHABLE_COLOR
    if mode == "sdf":
        d = dump.sdf[i]
        t = max(0.0, min(1.0, d / 20.0))
        return lerp_color((40, 20, 60), (220, 200, 255), t)
    # default: cost-to-goal heatmap
    if max_finite_cost <= 0.0:
        return COST_LOW
    t = c / max_finite_cost
    return lerp_color(COST_LOW, COST_HIGH, t)


def draw_line(canvas, w, h, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    x, y = x0, y0
    while True:
        if 0 <= x < w and 0 <= y < h:
            canvas[y][x] = color
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def render(dump: FlowFieldDump, mode: str, px_per_cell: int, draw_arrows: bool,
           arrow_stride: int, goal_cells) -> tuple[list[bytes], int, int]:
    w, h = dump.width, dump.height
    finite_costs = [c for c in dump.cost if c >= 0.0]
    max_finite_cost = max(finite_costs) if finite_costs else 0.0

    img_w = w * px_per_cell
    img_h = h * px_per_cell
    # canvas[y][x] = (r,g,b), row 0 = top of image. Grid cell (0,0) is the
    # bottom-left cell in world space by TissueMask convention, so flip y.
    canvas = [[WALL_COLOR for _ in range(img_w)] for _ in range(img_h)]

    for gy in range(h):
        for gx in range(w):
            col = cell_color(dump, gx, gy, mode, max_finite_cost)
            py0 = (h - 1 - gy) * px_per_cell
            px0 = gx * px_per_cell
            for py in range(py0, py0 + px_per_cell):
                row = canvas[py]
                for px in range(px0, px0 + px_per_cell):
                    row[px] = col

    for (gx, gy) in goal_cells:
        if 0 <= gx < w and 0 <= gy < h:
            py0 = (h - 1 - gy) * px_per_cell
            px0 = gx * px_per_cell
            for py in range(py0, py0 + px_per_cell):
                for px in range(px0, px0 + px_per_cell):
                    canvas[py][px] = GOAL_TINT

    if draw_arrows and px_per_cell >= 4:
        half = px_per_cell // 2
        arrow_len = max(2, int(px_per_cell * 0.4))
        for gy in range(0, h, arrow_stride):
            for gx in range(0, w, arrow_stride):
                i = dump.at(gx, gy)
                if not dump.walkable[i]:
                    continue
                dx, dy = dump.dir_x[i], dump.dir_y[i]
                if dx == 0.0 and dy == 0.0:
                    continue
                cx = gx * px_per_cell + half
                cy = (h - 1 - gy) * px_per_cell + half
                # Flip dy for image space (image y grows downward, world y grows upward).
                ex = cx + int(round(dx * arrow_len))
                ey = cy - int(round(dy * arrow_len))
                draw_line(canvas, img_w, img_h, cx, cy, ex, ey, ARROW_COLOR)

    return [bytes(b for px in row for b in px) for row in canvas], img_w, img_h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="path to an .imff dump")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output PNG path")
    ap.add_argument("--mode", choices=["cost", "mask", "sdf"], default="cost",
                     help="cell coloring: cost-to-goal heatmap (default), mask cost multiplier, or SDF")
    ap.add_argument("--px", type=int, default=6, help="pixels per cell (default 6)")
    ap.add_argument("--no-arrows", dest="arrows", action="store_false", help="skip direction arrows")
    ap.add_argument("--arrow-stride", type=int, default=2, help="draw one arrow every N cells (default 2)")
    ap.add_argument("--goal", action="append", default=[], metavar="X,Y",
                     help="mark a goal cell (repeatable), e.g. --goal 10,5")
    args = ap.parse_args()

    dump = FlowFieldDump(args.input)
    goal_cells = []
    for g in args.goal:
        gx, gy = g.split(",")
        goal_cells.append((int(gx), int(gy)))

    rows, img_w, img_h = render(dump, args.mode, max(1, args.px), args.arrows,
                                max(1, args.arrow_stride), goal_cells)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_png(args.out, rows, img_w, img_h)
    print(f"wrote {args.out} ({img_w}x{img_h}px, field {dump.width}x{dump.height} cells, "
          f"cell_size={dump.cell_size})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
