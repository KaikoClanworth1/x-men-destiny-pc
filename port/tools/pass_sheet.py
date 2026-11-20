"""One picture of a frame's passes (tools/passes.ps1's runs/<name>_pass<nn>.png):
each pass small, brightened so that its brightest pixel is white (the scene is
drawn in a dim linear range before tone mapping), its number and that gain in
the corner.

    python pass_sheet.py [name] [columns]   -> runs/<name>_passes.png
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "nat"
    columns = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    files = sorted(glob.glob(os.path.join(ROOT, "runs", f"{name}_pass[0-9][0-9].png")) +
                   glob.glob(os.path.join(ROOT, "runs", f"{name}_draw[0-9]*.png")) +
                   glob.glob(os.path.join(ROOT, "runs", f"{name}_ps*.png")))
    w, h = 400, 225
    rows = (len(files) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * w, rows * h))
    draw = ImageDraw.Draw(sheet)
    for i, path in enumerate(files):
        im = Image.open(path).convert("RGB")
        im = im.crop((8, 30, im.width - 8, im.height - 8))
        peak = max(hi for _, hi in im.getextrema())
        gain = 1.0 if peak >= 128 or peak == 0 else 255.0 / peak
        if gain > 1:
            im = im.point(lambda v: min(255, int(v * gain)))
        x, y = (i % columns) * w, (i // columns) * h
        sheet.paste(im.resize((w, h)), (x, y))
        draw.rectangle((x, y, x + 110, y + 13), fill=(0, 0, 0))
        draw.text((x + 3, y + 1), f"{os.path.basename(path)[len(name) + 1:-4][-12:]}  x{gain:.0f}", fill=(255, 255, 0))
    out = os.path.join(ROOT, "runs", f"{name}_passes.png")
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
