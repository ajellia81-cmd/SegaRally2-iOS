"""Car textures as one picture (an atlas), for painting a car's livery.

The game draws each car with several separate textures. This tool puts them
together in one PNG to paint, and cuts the painted PNG back into the files
the texture pack loads (textures/<hash>.png), at the resolution it was painted.

  python tools/car_atlas.py make  tools/cars/corolla.json corolla.png
      builds the atlas from the textures dumped by --dump-textures
      (out/textures/car/, else out/textures/)
  python tools/car_atlas.py split tools/cars/corolla.json corolla.png
      cuts it into textures/; the atlas may be enlarged (2x, 4x, any factor),
      as long as its proportions are kept

Every piece must stay where it is: the car finds its paint by position.
Pixels outside the pieces (left transparent by 'make') are not used.
Needs Pillow (pip install pillow).
"""
import json
import os
import sys

from PIL import Image

DUMPS = ["out/textures/car", "out/textures"]


def load_layout(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def find_dump(h):
    dirs = list(DUMPS)
    if os.path.isdir("out/cars"):   # one folder per car, copied from out/textures/car
        dirs += [os.path.join("out/cars", c) for c in sorted(os.listdir("out/cars"))]
    for d in dirs:
        p = os.path.join(d, h + ".png")
        if os.path.exists(p):
            return p
    sys.exit(f"{h}.png not found in {' or '.join(DUMPS)}: run the game with --dump-textures and drive this car first")


def make(layout, out):
    w, h = layout["size"]
    atlas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for t in layout["textures"]:
        if "from" in t:
            continue
        atlas.paste(Image.open(find_dump(t["hash"])).convert("RGBA"), (t["x"], t["y"]))
    atlas.save(out)
    print(f"{out}: {w}x{h}")


def split(layout, src, dst="textures"):
    w, h = layout["size"]
    atlas = Image.open(src)
    # A picture saved without transparency (JPEG, RGB PNG) keeps the game's
    # see-through parts (wheel spokes, grille): they are taken from the
    # dumped originals
    own_alpha = "A" in atlas.getbands() and atlas.getchannel("A").getextrema()[0] < 255
    atlas = atlas.convert("RGBA")
    k = atlas.width / w
    if abs(atlas.height / h - k) > 0.01:
        sys.exit(f"{src} is {atlas.width}x{atlas.height}: it must keep the proportions of {w}x{h}")
    # Any factor is accepted; a picture a few pixels off it (2.53x) is
    # brought to the nearest quarter so every piece's edges fall on pixels
    k = max(0.25, round(k * 4) / 4)
    want = (round(w * k), round(h * k))
    if atlas.size != want:
        print(f"{src}: {atlas.width}x{atlas.height} resized to {want[0]}x{want[1]} ({k}x)")
        atlas = atlas.resize(want, Image.LANCZOS)
    if not own_alpha:
        print("no transparency in the picture: the game's see-through parts are kept")
    os.makedirs(dst, exist_ok=True)

    def box(x, y, bw, bh):
        return tuple(round(v * k) for v in (x, y, x + bw, y + bh))

    for t in layout["textures"]:
        size = (round(t["w"] * k), round(t["h"] * k))
        if "from" in t:
            piece = atlas.crop(box(*t["from"])).resize(size, Image.BOX)
        else:
            piece = atlas.crop(box(t["x"], t["y"], t["w"], t["h"]))
            if not own_alpha:
                alpha = Image.open(find_dump(t["hash"])).convert("RGBA").getchannel("A")
                if alpha.getextrema()[0] < 255:
                    alpha = alpha.resize(size, Image.BILINEAR).point(lambda v: 255 if v >= 128 else 0)
                    piece.putalpha(alpha)
        piece.save(os.path.join(dst, t["hash"] + ".png"))
        print(f"{dst}/{t['hash']}.png  {size[0]}x{size[1]}  {t.get('what', '')}")


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("make", "split"):
        sys.exit(__doc__)
    layout = load_layout(sys.argv[2])
    if sys.argv[1] == "make":
        make(layout, sys.argv[3])
    else:
        split(layout, sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else "textures")


if __name__ == "__main__":
    main()
