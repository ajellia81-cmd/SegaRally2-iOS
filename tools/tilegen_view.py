"""Dumps the 2D layers (tilegen) captured in out/real3d/tilegen.bin to PNG.

Research tool to pin down the format before implementing it in the
renderer: each layer is a table of 64x64 tiles of 8x8 pixels.

Usage: python tools/tilegen_view.py [out/real3d/tilegen.bin] [out/tilegen]
"""
import os
import struct
import sys
import zlib

NAME_TABLES = [0xF8000, 0xFA000, 0xFC000, 0xFE000]
PALETTE = 0x100000
REGS = 0x180000


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def le32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def entry(d, table, tx, ty):
    """16-bit entry: each 32-bit (LE) word holds two tiles,
    the left one in the upper half."""
    w = le32(d, table + (ty * 64 + (tx & ~1)) * 2)
    return (w >> 16) & 0xFFFF if tx % 2 == 0 else w & 0xFFFF


def color(d, idx):
    v = le32(d, PALETTE + idx * 4)
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)


def pixel4(d, data, x, y):
    pattern = ((data & 0x3FFF) << 1) | ((data >> 15) & 1)
    word = le32(d, pattern * 32 + y * 4)
    nib = (word >> (28 - 4 * x)) & 15
    return nib, (data & 0x7FF0) | nib


def pixel8(d, data, x, y):
    pattern = data & 0x3FFF
    word = le32(d, pattern * 64 + y * 8 + (x & 4))
    byte = (word >> (24 - 8 * (x & 3))) & 0xFF
    return byte, (data & 0x7F00) | byte


def render(d, table, bpp):
    w = h = 512
    img = bytearray(w * h * 4)
    for ty in range(64):
        for tx in range(64):
            data = entry(d, table, tx, ty)
            for y in range(8):
                for x in range(8):
                    raw, idx = (pixel4 if bpp == 4 else pixel8)(d, data, x, y)
                    if raw == 0:
                        continue
                    o = ((ty * 8 + y) * w + tx * 8 + x) * 4
                    img[o:o + 4] = bytes(color(d, idx))
    return w, h, img


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "out/real3d/tilegen.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "out/tilegen"
    os.makedirs(out, exist_ok=True)
    d = open(src, "rb").read()
    for layer, table in enumerate(NAME_TABLES):
        if not any(d[table:table + 0x2000]):
            continue
        for bpp in (4, 8):
            w, h, img = render(d, table, bpp)
            path = os.path.join(out, f"layer{layer}_{bpp}bpp.png")
            write_png(path, w, h, img)
            print(path)


if __name__ == "__main__":
    main()
