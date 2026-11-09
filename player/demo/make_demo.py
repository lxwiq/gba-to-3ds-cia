# SPDX-License-Identifier: MIT
"""Adds the data tables of the gba2cia demo ROM.

The demo stands in for a game: it carries, at the places FireRed-engine
games use, a table of creature names, party icons and palettes (pointers at
0x138-0x147), a map name table (pointer at 0x0C0C94) and a small map with
its events. Every name and picture here is made up for the demo.

    python3 make_demo.py code.gba demo.gba
"""
import struct
import sys

ROM_SIZE = 0x100000
BASE = 0x08000000

NAMES = ["", "Mossling", "Emberkit", "Dewbit", "Pebblor", "Cloudle", "Thornet"]


def encode(text, length):
    out = bytearray()
    for c in text:
        if "A" <= c <= "Z":
            out.append(0xBB + ord(c) - 65)
        elif "a" <= c <= "z":
            out.append(0xD5 + ord(c) - 97)
        else:
            out.append(0x00)
    out = out[: length - 1] + b"\xff"
    return bytes(out.ljust(length, b"\xff"))


def rgb555(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


# Palettes: 0 transparent, 1 outline, 2 body, 3 light, 4 eyes, 5 accent, 6 white
PALETTES = [
    [(0, 0, 0), (24, 60, 32), (88, 184, 96), (152, 224, 144), (16, 24, 24), (64, 160, 48), (248, 248, 248)],
    [(0, 0, 0), (120, 40, 16), (240, 128, 48), (255, 192, 120), (24, 16, 16), (255, 224, 64), (248, 248, 248)],
    [(0, 0, 0), (24, 64, 128), (72, 152, 232), (168, 216, 255), (16, 24, 40), (40, 112, 200), (248, 248, 248)],
    [(0, 0, 0), (56, 56, 64), (136, 136, 144), (184, 184, 192), (24, 24, 32), (104, 96, 88), (248, 248, 248)],
    [(0, 0, 0), (120, 128, 152), (232, 236, 248), (255, 255, 255), (32, 32, 48), (200, 208, 232), (248, 248, 248)],
    [(0, 0, 0), (72, 24, 96), (160, 80, 200), (208, 152, 240), (24, 16, 32), (232, 120, 216), (248, 248, 248)],
]


def blank():
    return [[0] * 32 for _ in range(32)]


def blob(img, cx, cy, rx, ry):
    for y in range(32):
        for x in range(32):
            d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
            if d <= 1.0:
                img[y][x] = 2
            if 0.78 < d <= 1.0:
                img[y][x] = 1
    # light patch and eyes
    for y in range(32):
        for x in range(32):
            if ((x - cx + rx * 0.45) / (rx * 0.25)) ** 2 + ((y - cy + ry * 0.45) / (ry * 0.2)) ** 2 <= 1 and img[y][x] == 2:
                img[y][x] = 3
    ey = int(cy - ry * 0.05)
    for ex in (int(cx - rx * 0.35), int(cx + rx * 0.3)):
        img[ey][ex] = img[ey + 1][ex] = img[ey][ex + 1] = img[ey + 1][ex + 1] = 4
        img[ey][ex] = 6


def icon(kind):
    img = blank()
    if kind == 1:  # sprout on top
        blob(img, 16, 20, 10, 8)
        for y in range(5, 12):
            img[y][16] = 1
        for (x, y) in ((12, 6), (13, 5), (14, 6), (13, 7), (18, 6), (19, 5), (20, 6), (19, 7)):
            img[y][x] = 5
    elif kind == 2:  # spark on top
        blob(img, 16, 20, 10, 8)
        for (x, y) in ((16, 6), (15, 8), (16, 8), (17, 8), (14, 10), (16, 10), (18, 10)):
            img[y][x] = 5
    elif kind == 3:  # droplet
        blob(img, 16, 21, 9, 8)
        for y in range(6, 14):
            w = (y - 6) // 2
            for x in range(16 - w, 17 + w):
                img[y][x] = 2 if img[y][x] == 0 else img[y][x]
            img[y][16 - w] = 1
            img[y][16 + w] = 1
    elif kind == 4:  # rock
        for y in range(10, 29):
            for x in range(6, 27):
                if abs(x - 16) + abs(y - 19) * 1.2 <= 13:
                    img[y][x] = 2
        blob(img, 16, 20, 8, 7)
    elif kind == 5:  # cloud
        blob(img, 16, 20, 11, 7)
        for (cx, cy, r) in ((10, 14, 5), (20, 13, 6)):
            for y in range(32):
                for x in range(32):
                    if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and img[y][x] == 0:
                        img[y][x] = 3
    elif kind == 6:  # spiky orb
        blob(img, 16, 19, 9, 9)
        for (x, y) in ((16, 7), (16, 8), (6, 19), (7, 19), (25, 19), (26, 19), (9, 11), (23, 11), (9, 27), (23, 27)):
            img[y][x] = 5
    return img


def pack4bpp(img):
    out = bytearray()
    for t in range(16):
        tx, ty = (t % 4) * 8, (t // 4) * 8
        for y in range(8):
            for x in range(0, 8, 2):
                out.append(img[ty + y][tx + x] | (img[ty + y][tx + x + 1] << 4))
    return bytes(out)


def main(src, dst):
    rom = bytearray(open(src, "rb").read())
    assert len(rom) < 0x0C0000, "code too large for the data layout"
    rom += b"\xff" * (ROM_SIZE - len(rom))

    def put(off, data):
        rom[off : off + len(data)] = data

    def ptr(off):
        return struct.pack("<I", BASE + off)

    # Creature names, 11 bytes each.
    put(0x0C0000, b"".join(encode(n, 11) for n in NAMES))
    # Icons (two identical 32x32 frames each), icon table, palette ids, palettes.
    for i in range(1, len(NAMES)):
        gfx = pack4bpp(icon(i))
        put(0x0C4000 + i * 0x400, gfx + gfx)
        put(0x0C0200 + i * 4, ptr(0x0C4000 + i * 0x400))
        put(0x0C0300 + i, bytes([i - 1]))
    for p, colors in enumerate(PALETTES):
        data = b"".join(struct.pack("<H", rgb555(*c)) for c in colors).ljust(32, b"\x00")
        put(0x0C0500 + p * 32, data)
        put(0x0C0400 + p * 8, ptr(0x0C0500 + p * 32) + struct.pack("<HH", 0x1000 + p, 0))
    # Data header, as in the Generation III games.
    put(0x138, ptr(0x0C0200) + ptr(0x0C0300) + ptr(0x0C0400) + ptr(0x0C0000))
    # Map section names: one entry, "Demo Valley", for map section 0x58.
    put(0x0C0900, encode("Demo Valley", 16))
    put(0x0C0800, ptr(0x0C0900))
    put(0x0C0C94, ptr(0x0C0800))

    # A 40 x 30 map: trees around, a house, a pond, paths.
    W, H = 40, 30
    WALL = 0x0C00
    grid = [[1] * W for _ in range(H)]

    def block(x0, y0, x1, y1):
        for y in range(y0, y1):
            for x in range(x0, x1):
                grid[y][x] = WALL | 2

    block(0, 0, W, 2)
    block(0, H - 2, W, H)
    block(0, 0, 2, H)
    block(W - 2, 0, W, H)
    for y in range(2):
        for x in range(18, 22):
            grid[y][x] = 1  # exit to the north
    block(26, 6, 34, 12)   # house
    block(6, 18, 14, 24)   # pond
    block(4, 4, 7, 7)
    block(30, 20, 34, 25)  # tree clusters
    block(16, 22, 19, 26)
    put(0x0C2100, b"".join(struct.pack("<H", v) for row in grid for v in row))
    put(0x0C2000, struct.pack("<ii", W, H) + ptr(0x0C2100) + ptr(0x0C2100) + b"\x00" * 8)

    # Events: three people, two warps (house door, north exit).
    people = [(12, 8), (22, 14), (34, 16)]
    objects = b""
    for i, (x, y) in enumerate(people):
        objects += struct.pack("<BBBBhhBBHHHIHH", i + 1, 1, 0, 0, x, y, 3, 1, 0, 0, 0, 0, 0, 0)
    warps = struct.pack("<hhBBBB", 30, 12, 0, 0, 2, 1) + struct.pack("<hhBBBB", 20, 0, 0, 0, 3, 1)
    put(0x0C3100, objects)
    put(0x0C3200, warps)
    put(0x0C3000, struct.pack("<BBBB", len(people), 2, 0, 0) + ptr(0x0C3100) + ptr(0x0C3200) + b"\x00" * 8)

    open(dst, "wb").write(rom)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
