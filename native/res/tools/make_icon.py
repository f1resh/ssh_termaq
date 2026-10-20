"""Renders the SSHTermaq application icon (stdlib only) into native/res/windows/app.ico and native/res/icon-256.png;
with --mac into native/res/macos/app.icns instead (needs macOS's iconutil).

Design (same drawing as ui/src/components/welcome/Logo.tsx, in its 120x120 units): a near-black rounded square with
a neon tube — a magenta terminal-window outline whose corner grows the tail of a "Q" — and a pink ">_" prompt inside.
Each stroke is a soft Gaussian glow, a solid tube and a light core. Small sizes get thicker lines (at least about
1.5 px tube / 0.7 px core) so the shape survives at 16 px. Rendering: 4x4 supersampling of analytic distance fields.
"""
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.dirname(HERE)

UNITS = 120.0                 # design space of the logo
TILE = (0x08, 0x05, 0x0C)
FRAME = (0xE0, 0x40, 0xFB)    # neon magenta
FRAME_CORE = (0xFF, 0xE4, 0xFF)
PROMPT = (0xFF, 0x3D, 0xB8)   # neon pink
PROMPT_CORE = (0xFF, 0xE0, 0xF2)

# Shapes in design units.
WINDOW = (22.0, 24.0, 94.0, 92.0, 18.0)          # x0, y0, x1, y1, corner radius
TAIL = ((88.0, 86.0), (104.0, 102.0))
PROMPT_SEGMENTS = (((38.0, 46.0), (50.0, 57.0)), ((50.0, 57.0), (38.0, 68.0)), ((57.0, 69.0), (74.0, 69.0)))


def rounded_square_cover(x, y, size, radius):
    """Coverage test for a rounded square [0,size]^2."""
    if x < 0 or y < 0 or x > size or y > size:
        return False
    cx = min(max(x, radius), size - radius)
    cy = min(max(y, radius), size - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius


def seg_dist(px, py, a, b):
    (ax, ay), (bx, by) = a, b
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - (ax + t * vx), py - (ay + t * vy))


def outline_dist(px, py, rect):
    """Distance to the outline of a rounded rectangle."""
    x0, y0, x1, y1, r = rect
    hx, hy = (x1 - x0) / 2.0, (y1 - y0) / 2.0
    qx = abs(px - (x0 + hx)) - hx + r
    qy = abs(py - (y0 + hy)) - hy + r
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return abs(outside + min(max(qx, qy), 0.0) - r)


def frame_dist(px, py):
    return min(outline_dist(px, py, WINDOW), seg_dist(px, py, *TAIL))


def prompt_dist(px, py):
    return min(seg_dist(px, py, a, b) for a, b in PROMPT_SEGMENTS)


def over(dst, src, alpha):
    return [dst[i] * (1.0 - alpha) + src[i] * alpha for i in range(3)]


def render(n):
    ss = 4
    unit = UNITS / n                                   # design units per pixel
    tube = max(3.25, 0.75 * unit)                      # half widths, in design units
    core = max(1.4, 0.35 * unit)
    sigma = max(7.0, 1.0 * unit)
    groups = ((frame_dist, FRAME, FRAME_CORE), (prompt_dist, PROMPT, PROMPT_CORE))
    radius = 26.0 / UNITS * n
    pix = []
    for y in range(n):
        for x in range(n):
            inside = 0
            tube_hits = [0, 0]
            core_hits = [0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    if not rounded_square_cover(px, py, n, radius):
                        continue
                    inside += 1
                    ux, uy = px * unit, py * unit
                    for g, (dist, _, _) in enumerate(groups):
                        d = dist(ux, uy)
                        if d <= tube:
                            tube_hits[g] += 1
                        if d <= core:
                            core_hits[g] += 1
            if inside == 0:
                pix.append((0, 0, 0, 0))
                continue
            col = list(TILE)
            cx, cy = (x + 0.5) * unit, (y + 0.5) * unit
            for dist, color, _ in groups:                      # glow under everything
                d = max(0.0, dist(cx, cy) - tube)
                col = over(col, color, 0.55 * math.exp(-(d / sigma) ** 2))
            for g, (_, color, _) in enumerate(groups):         # solid tube
                col = over(col, color, 0.9 * tube_hits[g] / inside)
            for g, (_, _, light) in enumerate(groups):         # light core
                col = over(col, light, core_hits[g] / inside)
            alpha = inside / (ss * ss)
            pix.append((int(col[0] + 0.5), int(col[1] + 0.5), int(col[2] + 0.5), int(255 * alpha + 0.5)))
    return pix


def png_bytes(n, pix):
    raw = bytearray()
    for y in range(n):
        raw.append(0)
        for x in range(n):
            raw.extend(pix[y * n + x])

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def framed(n, margin):
    """The tile centred in an n x n transparent square with `margin` (a fraction of n) around it."""
    m = max(1, round(n * (1 - 2 * margin)))
    off = (n - m) // 2
    tile = render(m)
    pix = [(0, 0, 0, 0)] * (n * n)
    for y in range(m):
        for x in range(m):
            pix[(y + off) * n + x + off] = tile[y * m + x]
    return pix


def main_mac():
    """app.icns: macOS draws icons on a 1024 grid with the shape about 824 wide, so the tile gets a margin."""
    import subprocess
    import tempfile
    names = [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"), (128, "128x128"), (256, "128x128@2x"),
             (256, "256x256"), (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")]
    rendered = {}
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "app.iconset")
        os.mkdir(iconset)
        for n, name in names:
            if n not in rendered:
                rendered[n] = png_bytes(n, framed(n, 100 / 1024))
            with open(os.path.join(iconset, "icon_%s.png" % name), "wb") as f:
                f.write(rendered[n])
        out = os.path.join(OUT_DIR, "macos", "app.icns")
        subprocess.run(["iconutil", "-c", "icns", "-o", out, iconset], check=True)
    print("wrote app.icns")


def main():
    if "--mac" in sys.argv[1:]:
        return main_mac()
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = []
    for n in sizes:
        images.append((n, png_bytes(n, render(n))))
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    body = b""
    for n, data in images:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    with open(os.path.join(OUT_DIR, "windows", "app.ico"), "wb") as f:
        f.write(header + entries + body)
    with open(os.path.join(OUT_DIR, "icon-256.png"), "wb") as f:
        f.write(images[-1][1])
    print("wrote app.ico and icon-256.png")


if __name__ == "__main__":
    main()
