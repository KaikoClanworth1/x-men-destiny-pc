# blender --background --python tests/test_model.py -- <game folder> <archive name> <out.png> [front|side]
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_util as ru
from io_xmd import archive as ar
from io_xmd import build

args = sys.argv[sys.argv.index("--") + 1:]
game = ar.GameFiles(args[0])
ru.clear_scene()
ctx = build.Context(game)
objs, arms = build.import_models(ctx, game.find(args[1]))
print("XMD objects", [o.name for o in objs], "armatures", [a.name for a in arms])
for line in ctx.report:
    print("XMD note:", line)
for a in arms:
    a.hide_render = True
if arms:        # effect meshes that share the archive would hide the character
    for o in objs:
        if o.parent is None:
            o.hide_render = True
    objs = [o for o in objs if o.parent is not None]
ru.setup_render(args[2])
lo, hi = ru.bounds(objs)
print("XMD bounds", tuple(lo), tuple(hi))
view = args[3] if len(args) > 3 else "front"
d = {"front": (1.0, 0.25, 0.1), "side": (0.2, -1.0, 0.1), "back": (-1.0, -0.3, 0.1), "face": (1.0, 0.2, 0.0)}[view]
if view == "face":
    lo.z = hi.z - (hi.z - lo.z) * 0.17
    lo.y, hi.y = lo.y * 0.3, hi.y * 0.3
ru.add_camera(lo, hi, d, fit=0.8)
ru.render()
if len(args) > 4:
    import bpy
    bpy.ops.wm.save_as_mainfile(filepath=args[4])
