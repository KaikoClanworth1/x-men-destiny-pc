# Helpers for headless check renders (not part of the add-on).  By KaikoClanworth
import math

import bpy
from mathutils import Vector


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def bounds(objects):
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    dg = bpy.context.evaluated_depsgraph_get()
    for o in objects:
        if o.type != "MESH":
            continue
        e = o.evaluated_get(dg)
        for c in e.bound_box:
            w = e.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    return lo, hi


def setup_render(path, size=(900, 1100), engine=None, samples=16):
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = size
    sc.render.filepath = path
    sc.render.image_settings.file_format = "PNG"
    engines = [engine] if engine else ["BLENDER_EEVEE", "BLENDER_EEVEE_NEXT", "CYCLES"]
    for e in engines:
        try:
            sc.render.engine = e
            break
        except TypeError:
            continue
    if sc.render.engine == "CYCLES":
        sc.cycles.samples = samples
        sc.cycles.device = "CPU"
    world = bpy.data.worlds.new("w")
    sc.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (0.55, 0.6, 0.7, 1)
        bg.inputs[1].default_value = 0.9
    try:
        sc.view_settings.view_transform = "Standard"
    except TypeError:
        pass


def add_camera(lo, hi, direction=(0.9, -1.6, 0.35), fit=1.15, lens=60):
    sc = bpy.context.scene
    cam = bpy.data.cameras.new("cam")
    cam.lens = lens
    ob = bpy.data.objects.new("cam", cam)
    sc.collection.objects.link(ob)
    centre = (lo + hi) / 2
    size = max((hi - lo).length, 1e-3)
    d = Vector(direction).normalized()
    aspect = sc.render.resolution_x / sc.render.resolution_y
    fov = 2 * math.atan(18 / lens) * min(1.0, aspect)
    dist = size / 2 / math.tan(fov / 2) * fit
    ob.location = centre + d * dist
    ob.rotation_euler = (centre - ob.location).to_track_quat("-Z", "Y").to_euler()
    cam.clip_start = dist / 1000
    cam.clip_end = dist * 20
    sc.camera = ob
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 3.0
    so = bpy.data.objects.new("sun", sun)
    sc.collection.objects.link(so)
    so.rotation_euler = (centre - (centre + Vector((d.x + 0.5, d.y - 0.3, 1.2)))).to_track_quat("-Z", "Y").to_euler()
    return ob


def render():
    bpy.ops.render.render(write_still=True)
