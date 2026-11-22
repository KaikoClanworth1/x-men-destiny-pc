# blender --background --python tests/test_level.py -- <game> <out.png> <index cache> <dir x,y,z> <archive> [archive ...]
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy
import render_util as ru
from io_xmd import archive as ar
from io_xmd import build

args = sys.argv[sys.argv.index("--") + 1:]
game = ar.GameFiles(args[0])
ru.clear_scene()
ctx = build.Context(game)
ctx.index_cache = args[2]
objs = []
t0 = time.time()
for name in args[4:]:
    p = game.find(name)
    if not p:
        print("XMD missing archive", name)
        continue
    o = build.import_level(ctx, p)
    print("XMD level", name, len(o), "objects", round(time.time() - t0, 1), "s")
    objs += o
notes = {}
for line in ctx.report:
    key = line.split(":")[0] if line.startswith("texture") else line
    notes[key] = notes.get(key, 0) + 1
for k, v in list(notes.items())[:25]:
    print("XMD note:", k, "x", v)
print("XMD images", len([i for i in ctx.images.values() if i[0]]), "of", len(ctx.images), "materials", len(ctx.materials))
ru.setup_render(args[1], size=(1600, 900))
lo, hi = ru.bounds(objs)
print("XMD bounds", tuple(round(x, 1) for x in lo), tuple(round(x, 1) for x in hi))
d = tuple(float(x) for x in args[3].split(","))
ru.add_camera(lo, hi, d, fit=0.6, lens=35)
ru.render()
