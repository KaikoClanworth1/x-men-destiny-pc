# Renders one front view per archive: blender --background --python tests/sheet.py -- <game> <out folder> <archive> ...
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy
import render_util as ru
from io_xmd import archive as ar
from io_xmd import build

args = sys.argv[sys.argv.index("--") + 1:]
game = ar.GameFiles(args[0])
for name in args[2:]:
    ru.clear_scene()
    ctx = build.Context(game)
    ctx.use_stream_index = False
    try:
        objs, arms = build.import_models(ctx, game.find(name))
    except Exception as e:
        print("XMD FAILED", name, repr(e))
        continue
    notes = len(ctx.report)
    print("XMD", name, len(objs), "meshes", len(arms), "skeletons", notes, "notes", ctx.report[:3])
    if not objs:
        continue
    for a in arms:
        a.hide_render = True
    ru.setup_render(os.path.join(args[1], name.split(".")[0] + ".png"), size=(500, 700))
    lo, hi = ru.bounds(objs)
    ru.add_camera(lo, hi, (1.0, 0.35, 0.12), fit=0.62)
    ru.render()
