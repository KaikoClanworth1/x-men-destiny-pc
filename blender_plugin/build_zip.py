# Packs the add-on into dist/io_xmd-<version>.zip (install it with Blender's
# Edit > Preferences > Get Extensions > Install from Disk).  By KaikoClanworth
import os
import re
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "io_xmd")
manifest = open(os.path.join(src, "blender_manifest.toml")).read()
version = re.search(r'^version\s*=\s*"([^"]+)"', manifest, re.M).group(1)
os.makedirs(os.path.join(here, "dist"), exist_ok=True)
out = os.path.join(here, "dist", "io_xmd-%s.zip" % version)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        if name.endswith((".py", ".toml")):
            z.write(os.path.join(src, name), name)
    for extra in ("README.md", "LICENSE"):
        p = os.path.join(here, extra)
        if os.path.exists(p):
            z.write(p, extra)
print(out)
