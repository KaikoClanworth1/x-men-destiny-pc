# Installs the zip into a throw-away Blender profile and runs the three importers through
# their operators.  Run with BLENDER_USER_RESOURCES pointing at an empty folder:
# blender --background --python tests/test_install.py -- <zip> <game> <out folder> <model archive> <anim archive> <level archive>
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
zip_path, game, out = args[0], args[1], args[2]
assert os.environ.get("BLENDER_USER_RESOURCES"), "set BLENDER_USER_RESOURCES to a scratch folder"
bpy.ops.wm.read_factory_settings(use_empty=True)
try:
    r = bpy.ops.extensions.package_install_files(filepath=zip_path, repo="user_default", enable_on_install=True)
    print("XMD install (extension):", r)
except Exception as e:
    print("XMD extension install failed:", e)
    raise
import addon_utils
mods = [m.__name__ for m in addon_utils.modules() if "io_xmd" in m.__name__]
print("XMD modules:", mods)
print("XMD operators:", hasattr(bpy.ops.import_scene, "xmd_model"), "panel", hasattr(bpy.types, "XMD_PT_browser"))
r = bpy.ops.import_scene.xmd_model(filepath=os.path.join(game, args[3]))
print("XMD model import:", r, [o.name for o in bpy.data.objects])
arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
bpy.context.view_layer.objects.active = arm
r = bpy.ops.import_anim.xmd_clips(filepath=os.path.join(game, args[4]), first=0, count=5)
print("XMD anim import:", r, [(a.name, a["xmd_frames"]) for a in bpy.data.actions])
wm = bpy.context.window_manager
wm.xmd_game_folder = game
print("XMD refresh:", bpy.ops.xmd.refresh(), len(wm.xmd_archives), "archives listed")
print("XMD next clip:", bpy.ops.xmd.use_action(step=1), arm.animation_data.action.name)
n = len(bpy.data.objects)
r = bpy.ops.import_scene.xmd_level(filepath=os.path.join(game, args[5]), directory=game + os.sep,
                                   files=[{"name": args[5]}])
print("XMD level import:", r, len(bpy.data.objects) - n, "objects")
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "install_test.blend"))
print("XMD saved")
