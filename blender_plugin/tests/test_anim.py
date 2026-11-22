# blender --background --python tests/test_anim.py -- <game> <model archive> <anim archive> <clip index> <frame> <out.png>
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
ru.clear_scene()
ctx = build.Context(game)
objs, arms = build.import_models(ctx, game.find(args[1]))
from io_xmd import anim as an
infos = an.list_clips(game.open(game.find(args[2])))
print("XMD clips in archive", len(infos))
arm = [o.parent for o in objs if o.parent is not None][0]
arms = [arm]
acts = build.import_clips(ctx, arm, game.find(args[2]), indices={int(args[3])}, root_motion=False)
print("XMD actions", [(a.name, a["xmd_frames"], a["xmd_matched_bones"]) for a in acts])
for line in ctx.report:
    print("XMD note:", line)
build.assign_action(arms[0], acts[0])
frame = int(args[4])
bpy.context.scene.frame_set(frame)
arms[0].hide_render = True
for o in objs:
    if o.parent is None:
        o.hide_render = True
objs = [o for o in objs if o.parent is not None]
ru.setup_render(args[5])
bpy.context.view_layer.update()
lo, hi = ru.bounds(objs)
print("XMD bounds", tuple(lo), tuple(hi))
ru.add_camera(lo, hi, (1.0, -0.8, 0.15), fit=0.85)
ru.render()
