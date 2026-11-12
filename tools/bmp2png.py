"""Converts a 32-bit BMP (as written by SegaRally2.exe --shot) to PNG.

Usage: python tools/bmp2png.py out/capture.bmp out/capture.png
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from tilegen_view import write_png  # noqa: E402


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    h = abs(h)
    px = d[off:off + w * h * 4]
    rgba = bytearray(w * h * 4)
    rgba[0::4] = px[2::4]
    rgba[1::4] = px[1::4]
    rgba[2::4] = px[0::4]
    rgba[3::4] = bytes([255]) * (w * h)
    write_png(dst, w, h, rgba)


if __name__ == "__main__":
    main()
