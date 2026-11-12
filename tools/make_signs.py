"""The co-driver's signs (the panel with an arrow at the top of the race HUD)
as 2D replacements (src/video/pack2d.h), all at once.

The game keeps their layouts in the banked CROM (a table of named records:
name, then 6x6 name table entries relative to the sign tiles at 0x1900). This
tool puts each sign on screen in a copy of a race snapshot that shows one
(its 2D layer A, cells 28..33 x 11..16), has the game's renderer capture it
(--replay with LW_2D_CAPTURE) and makes its textures/2d/senal_<name>.txt.

  python tools/make_signs.py list
  python tools/make_signs.py capture out/snap/snap_3371.bin
      the captures (out/2d/senal_<name>.png/.cells) and the .txt files
  python tools/make_signs.py atlas [blue]
      out/2d/senales_atlas.png: every sign at 8x on a grid, to paint
  python tools/make_signs.py split atlas.png
      textures/2d/senal_<name>.png from a painted atlas (any scale)
Needs Pillow; capture needs build/cmake/SegaRally2.exe.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw

ROM = "data/banked_crom.bin"
TABLE = (0x224C000, 0x224F000)   # where the records are
TILE_BASE = 0x1900
COL, ROW = 28, 11                # where the HUD shows the sign (cells)
VRAM_SIZE = 0x190000
SCALE, GAP = 8, 4                # atlas: 8x, 4 original pixels between signs
PER_ROW = 6


def records():
    d = open(ROM, "rb").read()
    lo, hi = TABLE
    out, seen = [], set()
    for m in re.finditer(rb"[a-z_0-9]{3,}\x00", d[lo:hi]):
        name = m.group()[:-1].decode()
        end = lo + m.end()
        # the layout: after the record's header, which ends with its size, 6 x 6
        for k in range(end + (end & 1), end + 40, 2):
            if struct.unpack_from(">2H", d, k) == (6, 6):
                entries = struct.unpack_from(">36H", d, k + 4)
                break
        else:
            continue
        # names may carry the last letter of the record before them
        for real in (name, name[1:]):
            if real in KNOWN:
                name = real
        if name in seen or name not in KNOWN:   # (pieces of longer names)
            continue
        seen.add(name)
        out.append((name, [TILE_BASE + e for e in entries]))
    return out


KNOWN = {"bridge", "bump", "coution", "crest", "easy_l_r", "easy_left", "easy_r_l", "easy_right", "h_left",
         "h_right", "hazard", "k_l_r", "k_left", "k_r_l", "k_right", "keep_left", "keep_right", "kink",
         "kink_easy", "long_easy_left", "long_easy_right", "long_k_left", "long_k_right", "long_mid_left",
         "long_mid_right", "mid_l_r", "mid_left", "mid_r_l", "mid_right", "narrow", "open_h_left", "open_h_right",
         "sliply", "t_left", "t_right", "wide"}


def capture(snap):
    data = bytearray(open(snap, "rb").read())
    v0 = len(data) - VRAM_SIZE
    os.makedirs("out/2d", exist_ok=True)
    tmp = "out/2d/senal_tmp.bin"
    for name, entries in records():
        for k, e in enumerate(entries):
            c, r = COL + k % 6, ROW + k // 6
            off = v0 + 0xF8000 + (r * 32 + c // 2) * 4   # layer A, two entries a word (left one high)
            w = struct.unpack_from("<I", data, off)[0]
            w = (w & 0xFFFF0000) | e if c & 1 else (w & 0xFFFF) | e << 16
            struct.pack_into("<I", data, off, w)
        open(tmp, "wb").write(data)
        before = set(os.listdir("out/2d"))
        env = dict(os.environ, LW_BACKGROUND="1", LW_2D_CAPTURE="1")
        subprocess.run([os.path.abspath(os.path.join("build", "cmake", "SegaRally2.exe")), "--replay", tmp, "--shot", "30", "--volume", "0", "--music", "0"],
                       env=env, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        new = [f for f in set(os.listdir("out/2d")) - before if f.startswith("capture_")]
        base = next(f[:-4] for f in new if f.endswith(".png") and "_back" not in f and "_layer" not in f)
        for ext in (".png", ".cells"):
            shutil.move(f"out/2d/{base}{ext}", f"out/2d/senal_{name}{ext}")
        for f in new:
            if os.path.exists(f"out/2d/{f}"):
                os.remove(f"out/2d/{f}")
        subprocess.run([sys.executable, "tools/make_2d_replacement.py", f"out/2d/senal_{name}", str(COL * 8),
                        str(ROW * 8), "48", "48", f"senal_{name}"], check=True, stdout=subprocess.DEVNULL)
        os.remove(f"out/2d/senal_{name}_template.png")
        print(name)
    os.remove(tmp)


def blue(img):
    px = img.convert("RGB").getpixel((24, 4))
    return px[2] > 150 and px[0] < 100


def layout(names):
    cell = (48 + GAP) * SCALE
    rows = (len(names) + PER_ROW - 1) // PER_ROW
    return {n: ((i % PER_ROW) * cell + GAP * SCALE // 2, (i // PER_ROW) * cell + GAP * SCALE // 2)
            for i, n in enumerate(names)}, (PER_ROW * cell, rows * cell)


def atlas(only_blue):
    names = []
    for name, _ in records():
        img = Image.open(f"out/2d/senal_{name}.png").crop((COL * 8, ROW * 8, COL * 8 + 48, ROW * 8 + 48))
        if not only_blue or blue(img):
            names.append(name)
    where, size = layout(names)
    sheet = Image.new("RGBA", size, (0, 0, 0, 0))
    for n in names:
        img = Image.open(f"out/2d/senal_{n}.png").convert("RGBA").crop((COL * 8, ROW * 8, COL * 8 + 48, ROW * 8 + 48))
        sheet.paste(img.resize((48 * SCALE, 48 * SCALE), Image.NEAREST), where[n])
    sheet.save("out/2d/senales_atlas.png")
    json.dump(names, open("out/2d/senales_atlas.json", "w"), indent=1)
    # a copy with the names under each sign, as a guide
    guide = sheet.copy()
    d = ImageDraw.Draw(guide)
    for n, (x, y) in where.items():
        d.text((x, y + 48 * SCALE + 2), n, fill=(255, 255, 0, 255))
    guide.save("out/2d/senales_atlas_nombres.png")
    print(f"out/2d/senales_atlas.png: {len(names)} signs, {size[0]}x{size[1]}")


def split(path):
    names = json.load(open("out/2d/senales_atlas.json"))
    where, size = layout(names)
    art = Image.open(path).convert("RGBA")
    k = art.width / size[0]
    for n, (x, y) in where.items():
        box = [round(v * k) for v in (x, y, x + 48 * SCALE, y + 48 * SCALE)]
        art.crop(box).save(f"textures/2d/senal_{n}.png")
        print(f"textures/2d/senal_{n}.png")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "list":
        for n, e in records():
            print(n, " ".join("%04x" % x for x in e[:6]))
    elif cmd == "capture":
        capture(sys.argv[2])
    elif cmd == "atlas":
        atlas("blue" in sys.argv[2:])
    elif cmd == "split":
        split(sys.argv[2])
    else:
        sys.exit(__doc__)
