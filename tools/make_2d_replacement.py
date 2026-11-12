"""Prepares a 2D graphics replacement (src/video/pack2d.h) from a capture of
the foreground 2D layer (F12 in the game, or --shot N), e.g. the route map.

Usage:
  python tools/make_2d_replacement.py out/2d/capture_5200 X Y W H name [--keep X Y W H]

X Y W H is the rectangle in 2D pixels (496x384 screen), widened to whole 8x8
cells. --keep marks an area where the game's own pixels always stay on top
(text that changes, such as the map's hint line). Writes:
  textures/2d/<name>.txt            what the game looks for on screen
  out/2d/<name>_template.png        the original area at 4x, as a reference
Then draw the new picture (any resolution, same proportions, alpha allowed)
and save it as textures/2d/<name>.png. Needs Pillow.
"""
import os
import sys

from PIL import Image

CELLS_X, CELLS_Y = 62, 48

args = sys.argv[1:]
keep = None
if "--keep" in args:
    k = args.index("--keep")
    keep = [int(v) for v in args[k + 1:k + 5]]
    del args[k:k + 5]
if len(args) != 6:
    sys.exit(__doc__)
base, name = args[0], args[5]
x, y, w, h = (int(v) for v in args[1:5])
cx0, cy0 = x // 8, y // 8
cx1, cy1 = min(CELLS_X, (x + w + 7) // 8), min(CELLS_Y, (y + h + 7) // 8)

rows = [line.split() for line in open(base + ".cells")]
os.makedirs("textures/2d", exist_ok=True)
with open(f"textures/2d/{name}.txt", "w") as f:
    f.write(f"# 2D replacement made from {base}\n")
    f.write(f"rect {cx0} {cy0} {cx1 - cx0} {cy1 - cy0}\n")
    for cy in range(cy0, cy1):
        for cx in range(cx0, cx1):
            kept = keep and keep[0] <= cx * 8 + 7 and cx * 8 < keep[0] + keep[2] and \
                keep[1] <= cy * 8 + 7 and cy * 8 < keep[1] + keep[3]
            f.write(("0" if kept else rows[cy][cx]) + "\n")

img = Image.open(base + ".png").convert("RGBA").crop((cx0 * 8, cy0 * 8, cx1 * 8, cy1 * 8))
os.makedirs("out/2d", exist_ok=True)
img.resize((img.width * 4, img.height * 4), Image.LANCZOS).save(f"out/2d/{name}_template.png")
print(f"textures/2d/{name}.txt: cells {cx0},{cy0} {cx1 - cx0}x{cy1 - cy0} "
      f"(pixels {cx0 * 8},{cy0 * 8} {img.width}x{img.height})")
print(f"Template: out/2d/{name}_template.png ({img.width * 4}x{img.height * 4})")
print(f"Save the new picture as textures/2d/{name}.png")
