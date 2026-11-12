"""Renders the texture RAM dumped by SegaRally2.exe --shot (out/texture_ram.bin)
as ARGB1555, painting texels with the transparency bit set in magenta.

Usage: python tools/texture_view.py [out/texture_ram.bin] [out/texture_ram.png] [x y w h]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from tilegen_view import write_png  # noqa: E402


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "out/texture_ram.bin"
    dst = sys.argv[2] if len(sys.argv) > 2 else "out/texture_ram.png"
    x0, y0, w, h = (int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (0, 0, 2048, 2048)
    data = open(src, "rb").read()
    img = bytearray(w * h * 4)
    for y in range(h):
        row = struct.unpack_from("<%dH" % w, data, ((y0 + y) * 2048 + x0) * 2)
        for x, v in enumerate(row):
            o = (y * w + x) * 4
            if v & 0x8000:
                img[o:o + 4] = bytes((255, 0, 255, 255))
            else:
                img[o:o + 4] = bytes((((v >> 10) & 31) * 8, ((v >> 5) & 31) * 8, (v & 31) * 8, 255))
    write_png(dst, w, h, img)
    print(dst)


if __name__ == "__main__":
    main()
