#!/usr/bin/env python3
"""Draws the Arbiter icon (a traffic light: red over amber over green on a dark
post) with the standard library only, and writes icons/arbiter.png (256 px)
and icons/arbiter.ico (the same PNG wrapped as a Vista-style icon).
    python3 tools/make_icons.py
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "icons")
N = 256


def circle(px, cx, cy, r, color):
    r2 = r * r
    for y in range(max(0, int(cy - r - 1)), min(N, int(cy + r + 2))):
        for x in range(max(0, int(cx - r - 1)), min(N, int(cx + r + 2))):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if d <= r2:
                px[y][x] = color
            elif d <= (r + 1) ** 2:          # one pixel of edge blending
                a = 1.0 - (d ** 0.5 - r)
                old = px[y][x]
                px[y][x] = tuple(int(old[i] * (1 - a) + color[i] * a) for i in range(3)) + (max(old[3], int(255 * a)),)


def rounded_rect(px, x0, y0, x1, y1, r, color):
    for y in range(y0, y1):
        for x in range(x0, x1):
            inside = True
            for cx, cy in ((x0 + r, y0 + r), (x1 - r - 1, y0 + r), (x0 + r, y1 - r - 1), (x1 - r - 1, y1 - r - 1)):
                if (x < x0 + r or x >= x1 - r) and (y < y0 + r or y >= y1 - r):
                    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                        inside = False
            if inside:
                px[y][x] = color


def png(px):
    raw = b"".join(b"\x00" + bytes(c for row_px in row for c in row_px) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    px = [[(0, 0, 0, 0) for _ in range(N)] for _ in range(N)]
    post = (38, 42, 48, 255)
    rounded_rect(px, 78, 14, 178, 242, 28, post)
    rounded_rect(px, 86, 22, 170, 234, 22, (24, 26, 30, 255))
    circle(px, 128, 60, 30, (220, 50, 40, 255))      # red: held
    circle(px, 128, 128, 30, (240, 170, 30, 255))    # amber: TX disabled
    circle(px, 128, 196, 30, (60, 190, 90, 255))     # green: clear
    for cx, cy in ((128, 60), (128, 128), (128, 196)):   # a highlight on each lamp
        circle(px, cx - 9, cy - 9, 7, (255, 255, 255, 110))
    data = png(px)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "arbiter.png"), "wb") as f:
        f.write(data)
    # ICO with one PNG-compressed 256x256 image (Vista and later; MinGW windres accepts it)
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(data), 22) + data
    with open(os.path.join(OUT, "arbiter.ico"), "wb") as f:
        f.write(ico)
    print("wrote", os.path.join(OUT, "arbiter.png"), "and arbiter.ico")


if __name__ == "__main__":
    main()
