# blender --background --python tests/test_level_view.py -- <game> <out prefix> <index cache> <x,y,z game units> <yaw,yaw,..> <archive> [...]
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy
from mathutils import Euler, Vector
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
for name in args[5:]:
    p = game.find(name)
    if not p:
        print("XMD missing archive", name)
        continue
    o = build.import_level(ctx, p)
    print("XMD level", name, len(o), "objects", round(time.time() - t0, 1), "s")
    objs += o
notes = {}
for line in ctx.report:
    key = "texture not found" if line.startswith("texture") else line
    notes[key] = notes.get(key, 0) + 1
for k, v in list(notes.items())[:25]:
    print("XMD note:", k, "x", v)
print("XMD images", len([i for i in ctx.images.values() if i[0]]), "of", len(ctx.images), "materials", len(ctx.materials))
ru.setup_render(args[1] + ".png", size=(1280, 720))
sc = bpy.context.scene
cam = bpy.data.cameras.new("cam")
cam.lens = 22
cam.clip_start = 0.1
cam.clip_end = 5000
ob = bpy.data.objects.new("cam", cam)
sc.collection.objects.link(ob)
sc.camera = ob
pos = Vector(tuple(float(x) for x in args[3].split(","))) * ctx.scale
ob.location = pos
sun = bpy.data.lights.new("sun", "SUN")
sun.energy = 3.0
so = bpy.data.objects.new("sun", sun)
sc.collection.objects.link(so)
so.rotation_euler = (math.radians(50), 0, math.radians(30))
for yaw in args[4].split(","):
    ob.rotation_euler = Euler((math.radians(88), 0, math.radians(float(yaw))), "XYZ")
    sc.render.filepath = "%s_yaw%s.png" % (args[1], yaw)
    bpy.ops.render.render(write_still=True)
if os.environ.get("XMD_SAVE_BLEND"):
    bpy.ops.wm.save_as_mainfile(filepath=os.environ["XMD_SAVE_BLEND"])
