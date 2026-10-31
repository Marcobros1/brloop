"""Generates app/assets/icon/brloop.ico from the approved pixel design.

Standard library only. Usage:
    python tools\\make_icon.py

Every output pixel is the average of the 24x24 design grid under it, so the
ring and the play triangle stay connected at every size. The script also checks
that no output pixel which overlaps a drawn cell is left in the background
colour; any such pixel would show up as a hole and makes the script fail.
"""
import math
import os
import struct
import sys
import zlib

GRID = 24
CENTER = 11.5
BG = (0x23, 0x25, 0x3A)
FG = (0xF2, 0xEA, 0xD2)
ACCENT = (0xFF, 0x9D, 0x4D)
SIZES = [16, 32, 48, 256]
SUBSAMPLES = 8

TRIANGLE = ((9.2, 7.6), (9.2, 15.4), (15.6, 11.5))


def in_triangle(px, py, a, b, c):
    def side(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])

    p = (px, py)
    d1 = side(p, a, b)
    d2 = side(p, b, c)
    d3 = side(p, c, a)
    has_neg = d1 < 0 or d2 < 0 or d3 < 0
    has_pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (has_neg and has_pos)


def cell_color(x, y):
    px, py = x + 0.5, y + 0.5
    if in_triangle(px, py, *TRIANGLE):
        return ACCENT
    dx, dy = px - CENTER, py - CENTER
    dist = math.hypot(dx, dy)
    angle = math.degrees(math.atan2(dy, dx))
    in_gap = -55 < angle < -15
    if 7 <= dist <= 9.2 and not in_gap:
        return FG
    return BG


CELLS = [[cell_color(x, y) for x in range(GRID)] for y in range(GRID)]


def render(size):
    scale = GRID / size
    total = SUBSAMPLES * SUBSAMPLES
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = 0
            for j in range(SUBSAMPLES):
                v = (py + (j + 0.5) / SUBSAMPLES) * scale
                for i in range(SUBSAMPLES):
                    u = (px + (i + 0.5) / SUBSAMPLES) * scale
                    cr, cg, cb = CELLS[int(v)][int(u)]
                    r += cr
                    g += cg
                    b += cb
            row += bytes((r // total, g // total, b // total))
        rows.append(bytes(row))
    return rows


def count_holes(size, rows):
    scale = GRID / size
    holes = 0
    for py in range(size):
        y0, y1 = py * scale, (py + 1) * scale
        for px in range(size):
            x0, x1 = px * scale, (px + 1) * scale
            offset = px * 3
            if rows[py][offset:offset + 3] != bytes(BG):
                continue
            for gy in range(int(y0), min(GRID, math.ceil(y1))):
                for gx in range(int(x0), min(GRID, math.ceil(x1))):
                    if CELLS[gy][gx] != BG and gx < x1 and gx + 1 > x0 and gy < y1 and gy + 1 > y0:
                        holes += 1
                        break
                else:
                    continue
                break
    return holes


def png_bytes(size, rows):
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def ico_bytes(images):
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    data = b""
    for size, png in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
        data += png
    return out + entries + data


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, "app", "assets", "icon", "brloop.ico")
    images = []
    total_holes = 0
    for size in SIZES:
        rows = render(size)
        holes = count_holes(size, rows)
        total_holes += holes
        print(f"{size:>3} px: holes={holes}")
        images.append((size, png_bytes(size, rows)))
    if total_holes:
        print("FAILED: background pixels inside drawn shapes; icon not written")
        return 1
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "wb") as f:
        f.write(ico_bytes(images))
    print(f"Wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
