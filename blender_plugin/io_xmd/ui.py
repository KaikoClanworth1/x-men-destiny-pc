# Blender user interface: File > Import entries and the "XMD" sidebar panel.
# By KaikoClanworth
import os

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, IntProperty,
                       StringProperty)
from bpy_extras.io_utils import ImportHelper

from . import archive as ar
from . import build

_games = {}


def _game(folder):
    folder = os.path.normpath(folder)
    g = _games.get(folder)
    if g is None:
        g = _games[folder] = ar.GameFiles(folder)
    return g


def _context(folder, scale, use_alpha, wm=None):
    ctx = build.Context(_game(folder), scale=scale, use_alpha=use_alpha)
    try:
        cache_dir = bpy.utils.user_resource("CONFIG", path="xmd_importer", create=True)
        ctx.index_cache = os.path.join(cache_dir, "texture_index.json")
    except Exception:
        ctx.index_cache = None
    if wm is not None:
        def progress(i, n, name):
            if i == 0:
                print("XMD: indexing the game's texture archives (first time only)...")
            wm.progress_update(int(100 * i / max(1, n)))
        ctx.progress = progress
    return ctx


def _finish(op, ctx, what):
    seen = {}
    for line in ctx.report:
        key = "textures not found" if line.startswith("texture ") and "not found" in line else line
        seen[key] = seen.get(key, 0) + 1
    for key, n in list(seen.items())[:12]:
        op.report({"WARNING"}, "%s%s" % (key, " (%d)" % n if n > 1 else ""))
    op.report({"INFO"}, what)


class _Common:
    scale: FloatProperty(name="Scale", default=0.01, min=0.0001, max=100.0,
                         description="The game works in centimetres; 0.01 gives metres")
    use_alpha: BoolProperty(name="Use texture alpha", default=True,
                            description="Make materials transparent where the diffuse texture has alpha")


class XMD_OT_import_model(bpy.types.Operator, ImportHelper, _Common):
    """Import the models of an X-Men: Destiny archive (characters, props) with skeleton and materials"""
    bl_idname = "import_scene.xmd_model"
    bl_label = "Import XMD Model"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".gz"
    filter_glob: StringProperty(default="*.zzz.gz;*.zzz", options={"HIDDEN"})
    skeleton: BoolProperty(name="Skeleton and weights", default=True)
    bone_axis: BoolProperty(name="Bones along Y", default=True,
                            description="Turn the game's X-along bones into Blender's Y-along bones")

    def execute(self, context):
        folder = os.path.dirname(self.filepath)
        wm = context.window_manager
        wm.progress_begin(0, 100)
        try:
            ctx = _context(folder, self.scale, self.use_alpha, wm)
            ctx.bone_axis_fix = self.bone_axis
            objs, arms = build.import_models(ctx, self.filepath, self.skeleton)
        finally:
            wm.progress_end()
        if not objs:
            self.report({"ERROR"}, "No models in this archive")
            return {"CANCELLED"}
        for o in context.selected_objects:
            o.select_set(False)
        for o in objs + arms:
            o.select_set(True)
        context.view_layer.objects.active = arms[0] if arms else objs[0]
        _finish(self, ctx, "Imported %d model(s), %d skeleton(s)" % (len(objs), len(arms)))
        return {"FINISHED"}


class XMD_OT_import_anim(bpy.types.Operator, ImportHelper):
    """Import the animation clips of an X-Men: Destiny archive as actions on the active armature"""
    bl_idname = "import_anim.xmd_clips"
    bl_label = "Import XMD Animations"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".gz"
    filter_glob: StringProperty(default="*.zzz.gz;*.zzz", options={"HIDDEN"})
    first: IntProperty(name="First clip", default=0, min=0)
    count: IntProperty(name="Number of clips", default=20, min=0,
                       description="How many clips to import (0 = all of them; can be several hundred)")
    root_motion: BoolProperty(name="Root motion", default=True,
                              description="Keep the movement of the root bone (off = animate in place)")

    @classmethod
    def poll(cls, context):
        o = context.active_object
        return o is not None and o.type == "ARMATURE" and "xmd_bone_hashes" in o

    def execute(self, context):
        arm = context.active_object
        folder = os.path.dirname(self.filepath)
        ctx = _context(folder, float(arm.get("xmd_scale", 0.01)), True)
        from . import anim as an
        total = len(an.list_clips(ctx.game.open(self.filepath)))
        if not total:
            self.report({"ERROR"}, "No animation clips in this archive")
            return {"CANCELLED"}
        last = total if self.count == 0 else min(total, self.first + self.count)
        actions = build.import_clips(ctx, arm, self.filepath, set(range(self.first, last)), self.root_motion)
        actions = [a for a in actions if a.get("xmd_matched_bones", 0) > 0] or actions
        if not actions:
            self.report({"ERROR"}, "No clip could be read")
            return {"CANCELLED"}
        build.assign_action(arm, actions[0])
        sc = context.scene
        sc.render.fps = int(actions[0].get("xmd_fps", 30))
        sc.frame_start = 1
        sc.frame_end = max(1, int(actions[0].get("xmd_frames", 1)))
        _finish(self, ctx, "Imported %d of %d clips as actions (first one is active)" % (len(actions), total))
        return {"FINISHED"}


class XMD_OT_import_level(bpy.types.Operator, ImportHelper, _Common):
    """Import X-Men: Destiny level archives as a scene: static models placed at their positions"""
    bl_idname = "import_scene.xmd_level"
    bl_label = "Import XMD Map"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".gz"
    filter_glob: StringProperty(default="*.zzz.gz;*.zzz", options={"HIDDEN"})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement, options={"HIDDEN", "SKIP_SAVE"})
    directory: StringProperty(subtype="DIR_PATH", options={"HIDDEN"})
    unplaced: BoolProperty(name="Also models without a placement", default=False,
                           description="Import models the archive holds but does not place (at the origin)")

    def execute(self, context):
        folder = self.directory or os.path.dirname(self.filepath)
        names = [f.name for f in self.files if f.name] or [os.path.basename(self.filepath)]
        wm = context.window_manager
        wm.progress_begin(0, 100)
        total = 0
        try:
            ctx = _context(folder, self.scale, self.use_alpha, wm)
            for n in names:
                if "_tex." in n.lower():
                    continue
                total += len(build.import_level(ctx, os.path.join(folder, n), self.unplaced))
        finally:
            wm.progress_end()
        if not total:
            self.report({"ERROR"}, "No level geometry in the chosen archive(s)")
            return {"CANCELLED"}
        _finish(self, ctx, "Imported %d objects from %d archive(s)" % (total, len(names)))
        return {"FINISHED"}


# ------------------------------------------------------------------ sidebar browser

class XMD_ArchiveItem(bpy.types.PropertyGroup):
    name: StringProperty()
    size: IntProperty()


class XMD_UL_archives(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.label(text=item.name, icon="FILE_ARCHIVE")
        row.label(text="%.1f MB" % (item.size / 1048576.0))


class XMD_OT_refresh(bpy.types.Operator):
    """List the archives of the game folder"""
    bl_idname = "xmd.refresh"
    bl_label = "List archives"

    def execute(self, context):
        wm = context.window_manager
        folder = bpy.path.abspath(wm.xmd_game_folder)
        wm.xmd_archives.clear()
        if not os.path.isdir(folder):
            self.report({"ERROR"}, "Choose the game's folder first (the one with the .zzz.gz files)")
            return {"CANCELLED"}
        for n in sorted(os.listdir(folder), key=str.lower):
            low = n.lower()
            if low.endswith(".zzz.gz") and not low.endswith("_tex.zzz.gz"):
                item = wm.xmd_archives.add()
                item.name = n
                item.size = min(os.path.getsize(os.path.join(folder, n)), 2 ** 31 - 1)
        return {"FINISHED"}


class XMD_OT_browser_import(bpy.types.Operator):
    """Import the archive chosen in the list"""
    bl_idname = "xmd.browser_import"
    bl_label = "Import"
    bl_options = {"REGISTER", "UNDO"}
    kind: EnumProperty(items=[("MODEL", "Model", ""), ("ANIM", "Animations", ""), ("LEVEL", "Map", "")])

    def execute(self, context):
        wm = context.window_manager
        if not (0 <= wm.xmd_archive_index < len(wm.xmd_archives)):
            self.report({"ERROR"}, "Choose an archive in the list")
            return {"CANCELLED"}
        path = os.path.join(bpy.path.abspath(wm.xmd_game_folder), wm.xmd_archives[wm.xmd_archive_index].name)
        if self.kind == "MODEL":
            return bpy.ops.import_scene.xmd_model(filepath=path)
        if self.kind == "ANIM":
            if not XMD_OT_import_anim.poll(context):
                self.report({"ERROR"}, "Select an armature imported with this add-on first")
                return {"CANCELLED"}
            return bpy.ops.import_anim.xmd_clips(filepath=path, first=wm.xmd_clip_first, count=wm.xmd_clip_count)
        return bpy.ops.import_scene.xmd_level(filepath=path, directory=os.path.dirname(path) + os.sep)


class XMD_OT_use_action(bpy.types.Operator):
    """Play this action on the active armature"""
    bl_idname = "xmd.use_action"
    bl_label = "Use action"
    bl_options = {"REGISTER", "UNDO"}
    step: IntProperty(default=1)

    @classmethod
    def poll(cls, context):
        o = context.active_object
        return o is not None and o.type == "ARMATURE"

    def execute(self, context):
        arm = context.active_object
        acts = sorted((a for a in bpy.data.actions if "xmd_clip" in a), key=lambda a: a.name)
        if not acts:
            return {"CANCELLED"}
        cur = arm.animation_data.action if arm.animation_data else None
        i = (acts.index(cur) + self.step) % len(acts) if cur in acts else 0
        build.assign_action(arm, acts[i])
        context.scene.frame_start = 1
        context.scene.frame_end = max(1, int(acts[i].get("xmd_frames", 1)))
        self.report({"INFO"}, acts[i].name)
        return {"FINISHED"}


class XMD_PT_browser(bpy.types.Panel):
    bl_label = "X-Men: Destiny"
    bl_idname = "XMD_PT_browser"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "XMD"

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager
        layout.prop(wm, "xmd_game_folder")
        layout.operator("xmd.refresh", icon="FILE_REFRESH")
        layout.template_list("XMD_UL_archives", "", wm, "xmd_archives", wm, "xmd_archive_index", rows=12)
        col = layout.column(align=True)
        col.operator("xmd.browser_import", text="Import model", icon="OUTLINER_OB_ARMATURE").kind = "MODEL"
        col.operator("xmd.browser_import", text="Import as map", icon="WORLD").kind = "LEVEL"
        box = layout.box()
        box.label(text="Animations (onto the active armature)")
        row = box.row(align=True)
        row.prop(wm, "xmd_clip_first")
        row.prop(wm, "xmd_clip_count")
        box.operator("xmd.browser_import", text="Import animations", icon="ACTION").kind = "ANIM"
        row = box.row(align=True)
        row.operator("xmd.use_action", text="Previous clip", icon="TRIA_LEFT").step = -1
        row.operator("xmd.use_action", text="Next clip", icon="TRIA_RIGHT").step = 1


def _menu(self, context):
    self.layout.operator(XMD_OT_import_model.bl_idname, text="X-Men: Destiny model (.zzz.gz)")
    self.layout.operator(XMD_OT_import_anim.bl_idname, text="X-Men: Destiny animations (.zzz.gz)")
    self.layout.operator(XMD_OT_import_level.bl_idname, text="X-Men: Destiny map (.zzz.gz)")


_classes = (XMD_OT_import_model, XMD_OT_import_anim, XMD_OT_import_level, XMD_ArchiveItem, XMD_UL_archives,
            XMD_OT_refresh, XMD_OT_browser_import, XMD_OT_use_action, XMD_PT_browser)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(_menu)
    wm = bpy.types.WindowManager
    wm.xmd_game_folder = StringProperty(name="Game folder", subtype="DIR_PATH",
                                        description="Folder of your copy of the game (holds the .zzz.gz files)")
    wm.xmd_archives = CollectionProperty(type=XMD_ArchiveItem)
    wm.xmd_archive_index = IntProperty()
    wm.xmd_clip_first = IntProperty(name="First", default=0, min=0)
    wm.xmd_clip_count = IntProperty(name="Count", default=20, min=0, description="0 = all clips")


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu)
    for name in ("xmd_game_folder", "xmd_archives", "xmd_archive_index", "xmd_clip_first", "xmd_clip_count"):
        if hasattr(bpy.types.WindowManager, name):
            delattr(bpy.types.WindowManager, name)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
