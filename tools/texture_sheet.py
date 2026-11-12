"""Contact sheet of the textures dumped with SegaRally2.exe --dump-textures,
numbered, so a texture can be found by eye and its file name looked up.

Usage: python tools/texture_sheet.py [out/textures] [out/texture_sheet.png]
Prints "number file size" for every texture. Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw

src = sys.argv[1] if len(sys.argv) > 1 else "out/textures"
dst = sys.argv[2] if len(sys.argv) > 2 else "out/texture_sheet.png"
files = sorted(f for f in os.listdir(src) if f.endswith(".png"))
cell, cols = 128, 16
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * cell, max(1, rows) * (cell + 12)), (255, 0, 255))
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(files):
    im = Image.open(os.path.join(src, name)).convert("RGBA")
    print(i, name, "%dx%d" % im.size)
    im.thumbnail((cell, cell))
    bg = Image.new("RGBA", im.size, (255, 0, 255, 255))   # transparency shows as magenta
    bg.alpha_composite(im)
    x, y = (i % cols) * cell, (i // cols) * (cell + 12)
    sheet.paste(bg.convert("RGB"), (x, y))
    draw.text((x + 2, y + cell), str(i), fill=(0, 0, 0))
sheet.save(dst)
