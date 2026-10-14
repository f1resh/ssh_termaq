"""Renders the SSHTerm application icon (stdlib only) into native/res/app.ico and native/res/icon-256.png.

Design: rounded square with a diagonal blue gradient, a subtle inner highlight, and a white ">_" prompt.
Rendering uses 4x4 supersampling of analytic shapes (rounded rect, thick line segments, rectangle).
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.dirname(HERE)

TOP = (0x5B, 0x9C, 0xFF)     # light blue
BOTTOM = (0x2B, 0x4F, 0xD8)  # deep blue
GLYPH = (0xFF, 0xFF, 0xFF)


def rounded_rect_cover(x, y, size, radius):
    """Coverage test for a rounded square [0,size]^2."""
    if x < 0 or y < 0 or x > size or y > size:
        return False
    cx = min(max(x, radius), size - radius)
    cy = min(max(y, radius), size - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy)))
    dx, dy = px - (ax + t * vx), py - (ay + t * vy)
    return math.hypot(dx, dy)


def render(n):
    ss = 4
    pix = []
    s = float(n)
    radius = s * 0.22
    stroke = s * 0.085
    # ">" chevron
    a = (s * 0.24, s * 0.30)
    b = (s * 0.46, s * 0.50)
    c = (s * 0.24, s * 0.70)
    # "_" underscore
    ux0, ux1 = s * 0.52, s * 0.78
    uy0, uy1 = s * 0.64, s * 0.64 + stroke
    for y in range(n):
        for x in range(n):
            bg = 0
            fg = 0
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss
                    py = y + (sy + 0.5) / ss
                    if not rounded_rect_cover(px, py, s, radius):
                        continue
                    bg += 1
                    d = min(seg_dist(px, py, *a, *b), seg_dist(px, py, *b, *c))
                    if d <= stroke / 2 or (ux0 <= px <= ux1 and uy0 <= py <= uy1):
                        fg += 1
            total = ss * ss
            if bg == 0:
                pix.append((0, 0, 0, 0))
                continue
            t = (x + y) / (2.0 * n)
            col = [TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3)]
            # soft highlight on the upper half
            hl = max(0.0, 1.0 - y / (n * 0.55)) * 0.10
            col = [min(255, v + (255 - v) * hl) for v in col]
            fa = fg / bg
            col = [col[i] * (1 - fa) + GLYPH[i] * fa for i in range(3)]
            pix.append((int(col[0] + 0.5), int(col[1] + 0.5), int(col[2] + 0.5), int(255 * bg / total + 0.5)))
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


def main():
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
    with open(os.path.join(OUT_DIR, "app.ico"), "wb") as f:
        f.write(header + entries + body)
    with open(os.path.join(OUT_DIR, "icon-256.png"), "wb") as f:
        f.write(images[-1][1])
    print("wrote app.ico and icon-256.png")


if __name__ == "__main__":
    main()
