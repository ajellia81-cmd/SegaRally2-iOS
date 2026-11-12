"""The last ten seconds of the time left (the orange digits 9..0, pictures of
the game: 4x4 tiles at cells 31..34 x 4..7) as 2D replacements, painted in
one atlas.

  python tools/make_countdown.py capture
      replays the race snapshots of out/snap_rojos (one per digit, taken with
      LW_SNAP_AT as the time ran out) with LW_2D_CAPTURE and writes
      textures/2d/cuenta_N.txt, out/2d/cuenta_atlas.png (16x)
  python tools/make_countdown.py split painted.png
      textures/2d/cuenta_N.png from the painted atlas (any scale)
Needs Pillow and build/cmake/SegaRally2.exe.
"""
import os
import shutil
import subprocess
import sys

from PIL import Image

SNAPS = {9: 5420, 8: 5500, 7: 5545, 6: 5602, 5: 5659, 4: 5716, 3: 5773, 2: 5830, 1: 5887, 0: 6001}
X, Y, SIZE = 31 * 8, 4 * 8, 32
SCALE, GAP, PER_ROW = 16, 4, 5
ORDER = [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]


def where(i):
    cell = (SIZE + GAP) * SCALE
    return (i % PER_ROW) * cell + GAP * SCALE // 2, (i // PER_ROW) * cell + GAP * SCALE // 2


def atlas_size():
    cell = (SIZE + GAP) * SCALE
    return PER_ROW * cell, ((len(ORDER) + PER_ROW - 1) // PER_ROW) * cell


def capture():
    os.makedirs("out/2d", exist_ok=True)
    sheet = Image.new("RGBA", atlas_size(), (0, 0, 0, 0))
    exe = os.path.abspath(os.path.join("build", "cmake", "SegaRally2.exe"))
    for i, d in enumerate(ORDER):
        before = set(os.listdir("out/2d"))
        env = dict(os.environ, LW_BACKGROUND="1", LW_2D_CAPTURE="1")
        subprocess.run([exe, "--replay", f"out/snap_rojos/snap_{SNAPS[d]}.bin", "--shot", "30", "--volume", "0",
                        "--music", "0"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        new = [f for f in set(os.listdir("out/2d")) - before if f.startswith("capture_")]
        base = next(f[:-4] for f in new if f.endswith(".png") and "_back" not in f and "_layer" not in f)
        for ext in (".png", ".cells"):
            shutil.move(f"out/2d/{base}{ext}", f"out/2d/cuenta_{d}{ext}")
        for f in new:
            if os.path.exists(f"out/2d/{f}"):
                os.remove(f"out/2d/{f}")
        subprocess.run([sys.executable, "tools/make_2d_replacement.py", f"out/2d/cuenta_{d}", str(X), str(Y),
                        str(SIZE), str(SIZE), f"cuenta_{d}"], check=True, stdout=subprocess.DEVNULL)
        os.remove(f"out/2d/cuenta_{d}_template.png")
        img = Image.open(f"out/2d/cuenta_{d}.png").convert("RGBA").crop((X, Y, X + SIZE, Y + SIZE))
        sheet.paste(img.resize((SIZE * SCALE, SIZE * SCALE), Image.NEAREST), where(i))
        print(d)
    sheet.save("out/2d/cuenta_atlas.png")


def split(path):
    art = Image.open(path).convert("RGBA")
    k = art.width / atlas_size()[0]
    for i, d in enumerate(ORDER):
        x, y = where(i)
        box = [round(v * k) for v in (x, y, x + SIZE * SCALE, y + SIZE * SCALE)]
        art.crop(box).save(f"textures/2d/cuenta_{d}.png")
        print(f"textures/2d/cuenta_{d}.png")


if __name__ == "__main__":
    if sys.argv[1:2] == ["capture"]:
        capture()
    elif sys.argv[1:2] == ["split"] and len(sys.argv) > 2:
        split(sys.argv[2])
    else:
        sys.exit(__doc__)
