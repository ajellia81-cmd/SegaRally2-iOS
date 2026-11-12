"""Research into the Real3D Pro-1000 format based on the captured data.

Walks the scene tree (viewports -> nodes -> models), transforms the
vertices and draws the scene as a wireframe into a PNG to verify the format
before writing the Vulkan renderer.

Usage: python tools/real3d_view.py [out/real3d] [data/vrom.bin] [output.png]
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from tilegen_view import write_png  # noqa: E402


class Real3D:
    def __init__(self, folder, vrom_path):
        rd = lambda n: open(os.path.join(folder, n), "rb").read()  # noqa: E731
        self.lo = rd("culling_lo.bin")
        self.hi = rd("culling_hi.bin")
        self.poly = rd("polygon.bin")
        self.vrom = open(vrom_path, "rb").read()
        self.stats = {"viewports": 0, "nodes": 0, "models": 0, "polys": 0, "bad": 0}

    # Word (32-bit) addresses in the Real3D address space
    def cull(self, addr, i=0):
        addr &= 0xFFFFFF
        if 0x800000 <= addr < 0x840000:
            o = ((addr - 0x800000) + i) * 4
            return struct.unpack_from("<I", self.hi, o)[0] if o + 4 <= len(self.hi) else 0
        if addr < 0x100000:
            o = (addr + i) * 4
            return struct.unpack_from("<I", self.lo, o)[0] if o + 4 <= len(self.lo) else 0
        return None

    def cullf(self, addr, i):
        return struct.unpack("<f", struct.pack("<I", self.cull(addr, i)))[0]

    def model_word(self, addr):
        addr &= 0xFFFFFF
        if addr < 0x100000:
            o = addr * 4
            return struct.unpack_from("<I", self.poly, o)[0] if o + 4 <= len(self.poly) else 0
        o = (addr * 4) % len(self.vrom)   # 32 MB VROM mirrored across a 64 MB space
        return struct.unpack_from("<I", self.vrom, o)[0]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def transform(m, v):
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] for i in range(3)]


class Walker:
    def __init__(self, r3d, vertex_scale):
        self.r = r3d
        self.scale = vertex_scale
        self.tris = []      # (v0, v1, v2) in camera coordinates
        self.seen = set()

    def matrix(self, offset):
        b = self.matrix_base
        f = [self.r.cullf(b, offset * 12 + i) for i in range(12)]
        return [[f[3], f[4], f[5], f[0]], [f[6], f[7], f[8], f[1]], [f[9], f[10], f[11], f[2]], [0, 0, 0, 1]]

    def viewport(self, addr, depth=0):
        r = self.r
        if depth > 8 or r.cull(addr, 0) is None:
            return
        r.stats["viewports"] += 1
        vp = addr
        self.matrix_base = r.cull(vp, 0x16) & 0xFFFFFF
        disabled = r.cull(vp, 0) & 0x20
        info = {
            "addr": hex(addr), "flags": hex(r.cull(vp, 0)), "next": hex(r.cull(vp, 1)), "child": hex(r.cull(vp, 2)),
            "size": ((r.cull(vp, 0x14) & 0xFFFF) / 4, (r.cull(vp, 0x14) >> 16) / 4),
            "pos": ((r.cull(vp, 0x1A) & 0xFFFF) / 16, (r.cull(vp, 0x1A) >> 16) / 16),
            "matrix_base": hex(self.matrix_base), "prio": (r.cull(vp, 0) >> 3) & 3,
            "angles": [round(math.degrees(a), 1) for a in (
                math.atan2(r.cullf(vp, 12), r.cullf(vp, 13)), math.atan2(r.cullf(vp, 16), -r.cullf(vp, 17)),
                math.atan2(r.cullf(vp, 14), r.cullf(vp, 15)), math.atan2(r.cullf(vp, 18), -r.cullf(vp, 19)))],
        }
        print("viewport", info, "disabled" if disabled else "")
        if not disabled:
            self.viewport_info = info
            base = self.matrix(0)
            self.node_ptr(r.cull(vp, 2), base, 0)
        nxt = r.cull(vp, 1)
        if nxt != 0x01000000 and (nxt & 0xFFFFFF) and (nxt & 0xFFFFFF) != addr:
            self.viewport(nxt & 0xFFFFFF, depth + 1)

    def node_ptr(self, ptr, m, depth):
        kind, addr = (ptr >> 24) & 0xFF, ptr & 0xFFFFFF
        if addr == 0 or depth > 64:
            return
        if kind == 0x00:
            self.node(addr, m, depth)
        elif kind in (0x01, 0x03):
            self.model(addr, m)
        elif kind == 0x04:
            self.pointer_list(addr, m, depth)

    def node(self, addr, m, depth):
        r = self.r
        if r.cull(addr, 0) is None or (addr, depth) in self.seen and depth > 40:
            return
        r.stats["nodes"] += 1
        n = [r.cull(addr, i) for i in range(10)]
        mo = n[3] & 0xFFF
        x, y, z = (r.cullf(addr, i) for i in (4, 5, 6))
        local = m
        if n[0] & 0x10:
            local = mat_mul(m, [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]])
        elif mo:
            local = mat_mul(m, self.matrix(mo))
        if n[0] & 0x08:   # 4-entry LOD table
            lod = n[7] & 0xFFFFFF
            first = r.cull(lod, 0) or 0
            if n[3] & 0x20000000:
                self.node(first & 0xFFFFFF, local, depth + 1)
            else:
                self.model(first & 0xFFFFFF, local)
        else:
            self.node_ptr(n[7], local, depth + 1)
        if (n[0] & 7) != 6:
            self.node_ptr(n[8], m, depth + 1)

    def pointer_list(self, addr, m, depth):
        r = self.r
        items = []
        for i in range(4096):
            v = r.cull(addr, i)
            if v is None:
                break
            if v & 0x02000000:
                items.append(v)
                break
            if v == 0 or (v >> 24):
                break
            items.append(v)
        for v in reversed(items):
            a = v & 0xFFFFFF
            if a and a != 0x800800:
                self.node(a, m, depth + 1)

    def model(self, addr, m):
        r = self.r
        r.stats["models"] += 1
        prev = [None] * 4
        a = addr
        for _ in range(20000):
            h = [r.model_word(a + i) for i in range(7)]
            a += 7
            nverts = 4 if h[0] & 0x40 else 3
            verts = []
            for i in range(4):
                if (h[0] >> i) & 1:
                    verts.append(prev[i])
            while len(verts) < nverts:
                ix, iy, iz = (r.model_word(a + i) for i in range(3))
                a += 4
                sx = lambda v: ((v ^ 0x80000000) - 0x80000000) >> 8  # noqa: E731
                verts.append(transform(m, [sx(ix) * self.scale, sx(iy) * self.scale, sx(iz) * self.scale]))
            if any(v is None for v in verts):
                r.stats["bad"] += 1
            else:
                r.stats["polys"] += 1
                self.tris.append(verts[:3])
                if nverts == 4:
                    self.tris.append([verts[0], verts[2], verts[3]])
            for i in range(min(4, nverts)):
                prev[i] = verts[i]
            if h[1] & 4:   # last polygon
                break


def render(tris, info, path, w=496, h=384):
    img = bytearray(w * h * 4)
    for i in range(w * h):
        img[i * 4 + 3] = 255
    l, r_, t, b = (math.radians(a) for a in info["angles"])
    fx = w / (math.tan(abs(l)) + math.tan(abs(r_)))
    fy = h / (math.tan(abs(t)) + math.tan(abs(b)))
    cx, cy = math.tan(abs(l)) * fx, math.tan(abs(t)) * fy

    def proj(v):
        # OpenGL-style camera space: looking down -Z, Y up
        if not all(math.isfinite(c) for c in v) or not (-v[2] > 1e-4):
            return None
        d = -v[2]
        return int(cx + v[0] / d * fx), int(cy - v[1] / d * fy)

    def line(p, q):
        (x0, y0), (x1, y1) = p, q
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        if n > 2000:
            return
        for s in range(n + 1):
            x, y = x0 + (x1 - x0) * s // n, y0 + (y1 - y0) * s // n
            if 0 <= x < w and 0 <= y < h:
                o = (y * w + x) * 4
                img[o:o + 3] = b"\x40\xff\x40"

    for tri in tris:
        p = [proj(v) for v in tri]
        if None in p:
            continue
        line(p[0], p[1])
        line(p[1], p[2])
        line(p[2], p[0])
    write_png(path, w, h, img)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "out/real3d"
    vrom = sys.argv[2] if len(sys.argv) > 2 else "data/vrom.bin"
    out = sys.argv[3] if len(sys.argv) > 3 else "out/real3d_wireframe.png"
    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0 / 128
    r = Real3D(folder, vrom)
    wk = Walker(r, scale)
    wk.viewport(0x800000)
    print(r.stats, "triangles:", len(wk.tris))
    if wk.tris:
        zs = sorted(v[2] for t in wk.tris for v in t)
        print("z min/median/max:", zs[0], zs[len(zs) // 2], zs[-1])
        render(wk.tris, wk.viewport_info, out)
        print(out)


if __name__ == "__main__":
    main()
