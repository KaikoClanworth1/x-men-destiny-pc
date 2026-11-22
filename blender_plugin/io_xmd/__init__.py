# X-Men: Destiny (Xbox 360) importer for Blender.  By KaikoClanworth
bl_info = {
    "name": "X-Men: Destiny (Xbox 360) importer",
    "author": "KaikoClanworth",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "File > Import, 3D View > Sidebar > XMD",
    "description": "Imports models, textures, animations and maps from your own copy of the game",
    "category": "Import-Export",
}

try:
    import bpy
except ImportError:      # the readers also work outside Blender
    bpy = None

if bpy is not None:
    from . import ui

    def register():
        ui.register()

    def unregister():
        ui.unregister()
