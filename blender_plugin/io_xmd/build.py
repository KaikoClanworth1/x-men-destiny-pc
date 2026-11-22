# Turns parsed game data into Blender objects.  By KaikoClanworth
import math
import os

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

from . import archive as ar
from . import model as md
from . import names
from . import textures as tx

# Blender bones point along +Y, the game's (3ds Max Biped) along +X.
BONE_FIX = Matrix(((0.0, 1.0, 0.0, 0.0), (-1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.0, 0.0, 0.0, 1.0)))
# BONE_FIX maps Blender's bone space to the game's: game_x = blender_y.


class Context:
    """What one import needs: the game folder, open archives, caches, options."""

    def __init__(self, game, scale=0.01, use_alpha=True, bone_axis_fix=True, texture_dir=None):
        self.game = game
        self.scale = scale
        self.use_alpha = use_alpha
        self.bone_axis_fix = bone_axis_fix
        self.images = {}
        self.materials = {}
        self.texture_sources = []      # archives searched for textures, in order
        self.use_stream_index = True
        self.index_cache = None        # file that keeps the texture index between sessions
        self.progress = None
        self.meshes = {}
        self.collection = None         # where new objects are linked (None = the scene's)
        self.report = []

    def note(self, text):
        self.report.append(text)


# ------------------------------------------------------------------ textures

def _headers(a):
    heads = getattr(a, "_xmd_tex", None)
    if heads is None:
        heads = a._xmd_tex = tx.read_headers(a)
    return heads


def _data_chunk(a, t):
    return a.get(ar.C_STREAM_TEXTURE_DATA if t.streamed else ar.C_TEXTURE_DATA, t.data_name)


def _find_texture(ctx, name_hash):
    """(TextureInfo, data chunk) of the largest version found, or (None, None)."""
    best = None
    for a in ctx.texture_sources:
        t = _headers(a).get(name_hash)
        if t is None:
            continue
        c = _data_chunk(a, t)
        if c is None:
            for b in ctx.texture_sources:
                c = _data_chunk(b, t)
                if c is not None:
                    break
        if c is not None and (best is None or t.width * t.height > best[0].width * best[0].height):
            best = (t, c)
    if ctx.use_stream_index and (best is None or not best[0].streamed and best[0].width <= 32):
        # a level's own header is a 16x16 stand-in: the real texture is in some *_tex archive
        index = ctx.game.stream_index(ctx.index_cache, ctx.progress)
        fname = index.get(name_hash)
        if fname:
            a = ctx.game.open(os.path.join(ctx.game.folder, fname))
            t = _headers(a).get(name_hash)
            if t is not None:
                c = _data_chunk(a, t)
                if c is not None and (best is None or t.width * t.height > best[0].width * best[0].height):
                    best = (t, c)
    if not best:
        return None, None
    return best


def classify(img, fmt, normal_hint=False):
    """'normal_ag' (DXT5 with x in alpha), 'normal_rg', 'normal_rgb' or 'colour'."""
    small = img[::8, ::8].reshape(-1, 4).astype(np.float32)
    r, g, b, a = small.mean(0)
    if fmt == tx.F_DXN or fmt == tx.F_CTX1:
        return "normal_rg"
    if fmt == tx.F_DXT5 and abs(g - 128) < 45 and abs(a - 128) < 45 and (
            (r > 235 and b > 235) or (normal_hint and r > 150 and b > 150)):
        return "normal_ag"
    if normal_hint and b > 170 and abs(r - 128) < 45 and abs(g - 128) < 45:
        return "normal_rgb"
    if b > 200 and abs(r - 128) < 28 and abs(g - 128) < 28 and small[:, 2].min() > 100:
        return "normal_rgb"
    return "colour"


def get_image(ctx, name_hash, normal_hint=False):
    """Blender image for a texture name hash -> (image, kind, has_alpha) or (None, None, False)."""
    key = (name_hash, bool(normal_hint))
    if key in ctx.images:
        return ctx.images[key]
    result = (None, None, False)
    t, chunk = _find_texture(ctx, name_hash)
    if t is not None:
        w, h = t.width, t.height
        data = chunk.data
        # a streamed texture's archive copy may hold only the small mips
        full = tx.base_level_size(t)
        while len(data) < full and w > 4 and h > 4 and chunk.size < t.size:
            w //= 2
            h //= 2
            full = tx.base_level_size_wh(t, w, h)
        img = tx.decode(t, data, w, h) if not t.depth or t.depth <= 1 else None
        if t.kind != 1 and img is None:
            ctx.note("texture %08X: kind %d not handled" % (name_hash, t.kind))
        if img is not None:
            kind = classify(img, t.format, normal_hint)
            f = img.astype(np.float32) / 255.0
            has_alpha = False
            if kind == "normal_ag":
                x = f[..., 3] * 2 - 1
                y = f[..., 1] * 2 - 1
            elif kind == "normal_rg":
                x = f[..., 0] * 2 - 1
                y = f[..., 1] * 2 - 1
            if kind in ("normal_ag", "normal_rg"):
                z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
                f = np.stack([x * 0.5 + 0.5, y * 0.5 + 0.5, z * 0.5 + 0.5, np.ones_like(x)], -1)
            else:
                a = img[..., 3]
                low = float((a < 128).mean())
                has_alpha = bool(a.min() < 250) and 0.005 < low < 0.995
            image = bpy.data.images.new("xmd_%08X" % name_hash, w, h, alpha=True)
            image.pixels.foreach_set(np.ascontiguousarray(f[::-1]).ravel())
            if kind != "colour":
                image.colorspace_settings.name = "Non-Color"
            image.pack()
            image["xmd_format"] = "%08X" % t.format_word
            result = (image, kind, has_alpha)
    else:
        ctx.note("texture %08X not found" % name_hash)
    ctx.images[key] = result
    return result


# ------------------------------------------------------------------ materials

def _role(slot):
    """What a texture slot name means: diffuse / normal / spec / emissive / alpha / other."""
    low = slot.lower()
    if low.startswith("slot_"):
        return None
    if "detail" in low or "lens" in low or "noise" in low or "mask" in low and "reflection" in low:
        return "other"
    if "diff" in low or "colour" in low or "color" in low or "albedo" in low:
        return "diffuse"
    if "norm" in low or "nrm" in low or "bump" in low:
        return "normal"
    if "spec" in low or "gloss" in low:
        return "spec"
    if "emis" in low or "glow" in low:
        return "emissive"
    if "alpha" in low or "opacity" in low:
        return "alpha"
    return "other"


def build_material(ctx, archive, material_id, mats):
    key = (archive.path, material_id)
    if key in ctx.materials:
        return ctx.materials[key]
    mat = bpy.data.materials.new("xmd_mat_%08X" % material_id)
    ctx.materials[key] = mat
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = None
    for n in nt.nodes:
        if n.type == "BSDF_PRINCIPLED":
            bsdf = n
    if bsdf is None:
        return mat
    bsdf.inputs["Roughness"].default_value = 0.6
    m = mats.get(material_id)
    if m is None:
        return mat
    for k, v in m.params.items():
        mat["xmd_" + k] = list(v)
    loaded = []
    for i, th in enumerate(m.textures):
        slot = names.slot_name(m.slots[i]) if i < len(m.slots) else "slot%d" % i
        image, kind, has_alpha = get_image(ctx, th, _role(slot) == "normal")
        if image is not None:
            loaded.append([slot, image, kind, has_alpha, th, _role(slot)])
    effect = any(e[0].lower().startswith(("smoke", "fog", "dust", "steam")) for e in loaded)
    picked = {}
    for entry in loaded:                      # named slots first, lowest number wins (diff01 < diff02)
        slot, image, kind, has_alpha, th, role = entry
        if role in ("diffuse", "spec", "emissive", "alpha") and kind == "colour" and role not in picked:
            picked[role] = entry
        if role == "normal" and kind != "colour" and role not in picked:
            picked[role] = entry
    if "normal" not in picked:
        for entry in loaded:
            if entry[2] != "colour" and entry[5] in (None, "normal"):
                picked["normal"] = entry
                break
    if "diffuse" not in picked:
        # unnamed slots: the first reasonably large colour texture that is not black
        best = None
        for entry in loaded:
            slot, image, kind, has_alpha, th, role = entry
            if kind != "colour" or role not in (None, "other") or entry in picked.values():
                continue
            if best is None:
                best = entry
            if role is None and image.size[0] * image.size[1] >= 128 * 128:
                best = entry
                break
        if best is not None:
            picked["diffuse"] = best
    if effect:
        # smoke / fog cards: additive effects in the game, nearly invisible here
        mat["xmd_effect"] = True
        bsdf.inputs["Alpha"].default_value = 0.04
        _alpha_mode(mat)
        picked = {}
    y = 300
    for entry in loaded:
        slot, image, kind, has_alpha, th, role = entry
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = image
        node.label = "%s (%08X)" % (slot, th)
        node.location = (-700, y)
        y -= 280
        if picked.get("diffuse") is entry:
            nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
            if has_alpha and ctx.use_alpha and "alpha" not in picked:
                nt.links.new(node.outputs["Alpha"], bsdf.inputs["Alpha"])
                _alpha_mode(mat)
        elif picked.get("normal") is entry:
            nm = nt.nodes.new("ShaderNodeNormalMap")
            nm.location = (-350, y + 280)
            nt.links.new(node.outputs["Color"], nm.inputs["Color"])
            nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
        elif picked.get("spec") is entry:
            image.colorspace_settings.name = "Non-Color"
            target = bsdf.inputs.get("Specular IOR Level") or bsdf.inputs.get("Specular")
            if target is not None:
                nt.links.new(node.outputs["Color"], target)
        elif picked.get("emissive") is entry:
            target = bsdf.inputs.get("Emission Color") or bsdf.inputs.get("Emission")
            if target is not None:
                nt.links.new(node.outputs["Color"], target)
                if bsdf.inputs.get("Emission Strength") is not None:
                    bsdf.inputs["Emission Strength"].default_value = 1.0
        elif picked.get("alpha") is entry and ctx.use_alpha:
            image.colorspace_settings.name = "Non-Color"
            nt.links.new(node.outputs["Color"], bsdf.inputs["Alpha"])
            _alpha_mode(mat)
    return mat


def _alpha_mode(mat):
    try:
        mat.surface_render_method = "DITHERED"
    except AttributeError:
        mat.blend_method = "HASHED"


# ------------------------------------------------------------------ skeleton

def quat(q):
    return Quaternion((q[3], q[0], q[1], q[2]))


def bone_world_matrices(sk, scale):
    world = []
    for b in sk.bones:
        local = Matrix.Translation(Vector(b.pos) * scale) @ quat(b.rot).to_matrix().to_4x4()
        world.append(world[b.parent] @ local if b.parent >= 0 else local)
    return world


def build_armature(ctx, sk, name):
    arm = bpy.data.armatures.new(name)
    obj = bpy.data.objects.new(name, arm)
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    world = bone_world_matrices(sk, ctx.scale)
    fix = BONE_FIX if ctx.bone_axis_fix else Matrix.Identity(4)
    children = {}
    for i, b in enumerate(sk.bones):
        children.setdefault(b.parent, []).append(i)
    ebones = []
    bone_names = []
    used = set()
    for i, b in enumerate(sk.bones):
        n = names.bone_name(b.name_hash)
        if n in used:
            n = "%s_%d" % (n, i)
        used.add(n)
        bone_names.append(n)
        eb = arm.edit_bones.new(n)
        length = 0.0
        for c in children.get(i, []):
            length = max(length, (world[c].translation - world[i].translation).length)
        if length < 1e-4:
            length = 3.0 * ctx.scale
        length = min(length, 30.0 * ctx.scale)
        eb.head = (0, 0, 0)
        eb.tail = (0, length, 0)
        eb.matrix = world[i] @ fix
        ebones.append(eb)
    for i, b in enumerate(sk.bones):
        if b.parent >= 0:
            ebones[i].parent = ebones[b.parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    obj["xmd_skeleton"] = "%08X" % sk.id
    obj["xmd_bone_hashes"] = ["%08X" % b.name_hash for b in sk.bones]
    obj["xmd_bone_axis_fix"] = bool(ctx.bone_axis_fix)
    obj["xmd_scale"] = ctx.scale
    for i, b in enumerate(sk.bones):
        arm.bones[bone_names[i]]["xmd_hash"] = "%08X" % b.name_hash
    obj.show_in_front = True
    return obj, bone_names


# ------------------------------------------------------------------ meshes

def build_mesh(ctx, archive, m, mats, name, skeleton=None, armature=None, bone_names=None):
    """One Blender mesh object for a parsed model (all sections, one material slot each)."""
    pos = m.streams.get(md.VF_HALF4)
    if pos is None:
        pos = m.streams.get("pos")
    if pos is None or m.indices is None:
        ctx.note("model %08X: no positions" % m.name)
        return None
    pos = pos[:, :3] * ctx.scale
    uv = m.streams.get(md.VF_HALF2)
    if uv is None:
        uv = m.streams.get("uv")
    frame = m.streams.get(md.VF_FRAME)
    normals = frame[:, 2, :] if frame is not None else m.streams.get("normal")
    colour = m.streams.get(md.VF_COLOUR)
    tris = []
    tri_mat = []
    slots = []
    for s in m.sections:
        a = s.index_start
        t = m.indices[a:a + s.tri_count * 3]
        t = t[:len(t) // 3 * 3].reshape(-1, 3)
        if not len(t):
            continue
        if s.material not in slots:
            slots.append(s.material)
        tris.append(t)
        tri_mat.append(np.full(len(t), slots.index(s.material), np.int32))
    if not tris:
        return None
    tris = np.concatenate(tris)
    tri_mat = np.concatenate(tri_mat)
    ok = (tris.max(1) < len(pos)) & (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    tris = tris[ok][:, ::-1]            # the game's triangles are clockwise
    tri_mat = tri_mat[ok]
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(pos))
    mesh.vertices.foreach_set("co", pos.astype(np.float32).ravel())
    n = len(tris)
    mesh.loops.add(n * 3)
    mesh.polygons.add(n)
    mesh.loops.foreach_set("vertex_index", tris.astype(np.int32).ravel())
    mesh.polygons.foreach_set("loop_start", np.arange(0, n * 3, 3, dtype=np.int32))
    mesh.polygons.foreach_set("loop_total", np.full(n, 3, np.int32))
    mesh.polygons.foreach_set("material_index", tri_mat)
    mesh.polygons.foreach_set("use_smooth", np.ones(n, bool))
    flat = tris.ravel()
    if uv is not None:
        layer = mesh.uv_layers.new(name="UVMap")
        u = uv[flat].astype(np.float32).copy()
        u[:, 1] = 1.0 - u[:, 1]
        layer.data.foreach_set("uv", u.ravel())
    uv2 = m.streams.get("uv2")
    if uv2 is not None:
        layer = mesh.uv_layers.new(name="UVMap2")
        u = uv2[flat].astype(np.float32).copy()
        u[:, 1] = 1.0 - u[:, 1]
        layer.data.foreach_set("uv", u.ravel())
    if colour is not None and colour.min() < 255:
        attr = mesh.color_attributes.new("Col", "BYTE_COLOR", "POINT")
        c = colour.astype(np.float32) / 255.0
        attr.data.foreach_set("color", c[:, [1, 2, 3, 0]].ravel())     # stored A R G B
    mesh.update()
    mesh.validate(clean_customdata=False)
    if normals is not None and len(mesh.vertices) == len(pos):
        nn = normals.astype(np.float32)
        ln = np.linalg.norm(nn, axis=1, keepdims=True)
        nn = np.where(ln > 1e-5, nn / np.maximum(ln, 1e-5), np.array([[0, 0, 1]], np.float32))
        try:
            mesh.normals_split_custom_set_from_vertices(nn.tolist())
        except Exception as e:      # keep the mesh even if a build rejects custom normals
            ctx.note("normals not set: %s" % e)
    for mid in slots:
        mesh.materials.append(build_material(ctx, archive, mid, mats))
    obj = bpy.data.objects.new(name, mesh)
    (ctx.collection or bpy.context.scene.collection).objects.link(obj)
    obj["xmd_model"] = "%08X" % m.name
    bi = m.streams.get(md.VF_UBYTE4)
    bw = m.streams.get(md.VF_UBYTE4N)
    if skeleton is not None and armature is not None and bi is not None and bw is not None:
        groups = {}
        nb = len(skeleton.bones)
        for s in m.sections:
            first = s.palette_offset // 2
            pal = skeleton.palette[first:first + s.palette_count]
            if not pal:
                continue
            pal = np.array(pal, np.int64)
            v0, v1 = s.min_vertex, min(s.max_vertex + 1, len(pos))
            idx = np.minimum(bi[v0:v1].astype(np.int64), len(pal) - 1)
            bones = pal[idx]
            w = bw[v0:v1].astype(np.float32) / 255.0
            for k in range(4):
                sel = np.nonzero(w[:, k] > 0)[0]
                for vi, b, wt in zip((sel + v0).tolist(), bones[sel, k].tolist(), w[sel, k].tolist()):
                    if b < nb:
                        groups.setdefault(b, {}).setdefault(round(wt, 4), []).append(vi)
        for b in sorted(groups):
            vg = obj.vertex_groups.new(name=bone_names[b])
            for wt, verts in groups[b].items():
                vg.add(verts, wt, "ADD")
        obj.parent = armature
        mod = obj.modifiers.new("Armature", "ARMATURE")
        mod.object = armature
    return obj


def import_models(ctx, path, with_skeleton=True, collection_name=None):
    """Import every model of one archive.  Returns (objects, armatures)."""
    archive = ctx.game.open(path)
    ctx.texture_sources = [archive]
    for p in ctx.game.texture_archives(path):
        ctx.texture_sources.append(ctx.game.open(p))
    models = md.read_models(archive)
    skeletons = md.read_skeletons(archive)
    mats = md.read_materials(archive)
    base = os.path.basename(path).split(".")[0]
    objects = []
    armatures = {}
    for m in models:
        if not m.vertex_count:
            continue
        sk = skeletons.get(m.skeleton_id) if with_skeleton else None
        arm = bone_names = None
        if sk is not None and md.VF_UBYTE4 in m.streams:
            if sk.id not in armatures:
                armatures[sk.id] = build_armature(ctx, sk, base + "_skeleton")
            arm, bone_names = armatures[sk.id]
        name = base if len(models) == 1 else "%s_%08X" % (base, m.name)
        obj = build_mesh(ctx, archive, m, mats, name, sk, arm, bone_names)
        if obj:
            objects.append(obj)
    return objects, [a for a, _ in armatures.values()]


# ------------------------------------------------------------------ animation

def _new_fcurves(action, obj):
    """Returns a function making f-curves in `action` for `obj` (old and layered action API)."""
    if hasattr(action, "layers") and hasattr(action, "slots"):
        slot = action.slots.new(id_type="OBJECT", name=obj.name)
        layer = action.layers.new("Layer")
        strip = layer.strips.new(type="KEYFRAME")
        bag = strip.channelbag(slot, ensure=True)

        def make(path, index, group):
            fc = bag.fcurves.new(path, index=index)
            try:
                g = bag.groups.get(group) or bag.groups.new(group)
                fc.group = g
            except Exception:
                pass
            return fc
        return make, slot

    def make_old(path, index, group):
        return action.fcurves.new(path, index=index, action_group=group)
    return make_old, None


def _fill(fc, frames, values):
    n = len(frames)
    fc.keyframe_points.add(n)
    co = np.empty((n, 2), np.float32)
    co[:, 0] = frames
    co[:, 1] = values
    fc.keyframe_points.foreach_set("co", co.ravel())
    fc.keyframe_points.foreach_set("interpolation", np.full(n, 1, np.int32))   # LINEAR
    fc.update()


def import_clip(ctx, armature, clip, name, root_motion=True, frame_start=1):
    """Make an action for `armature` from a decoded clip.  Returns the action or None."""
    from . import anim as an
    hashes = list(armature.get("xmd_bone_hashes", []))
    if not hashes:
        return None
    scale = float(armature.get("xmd_scale", ctx.scale))
    fix = BONE_FIX if armature.get("xmd_bone_axis_fix", True) else Matrix.Identity(4)
    fixq = fix.to_quaternion()
    fixq_inv = fixq.inverted()
    by_hash = {}
    for b in armature.data.bones:
        hv = b.get("xmd_hash")
        if hv:
            by_hash[int(hv, 16)] = b
    action = bpy.data.actions.new(name)
    make, slot = _new_fcurves(action, armature)
    matched = 0
    for h, tr in clip.tracks.items():
        bone = by_hash.get(h)
        if bone is None:
            continue
        matched += 1
        # the bone's rest transform relative to its parent, in game space
        rest = bone.matrix_local @ fix.inverted()
        if bone.parent:
            rest = (bone.parent.matrix_local @ fix.inverted()).inverted() @ rest
        rest_q = rest.to_quaternion()
        rest_q_inv = rest_q.inverted()
        rest_p = rest.translation
        is_root = bone.parent is None
        base = 'pose.bones["%s"]' % bone.name.replace('"', '\\"')
        first = clip.first.get(h)
        rot = tr.get("rot")
        if rot is not None:
            frames, v = rot
            out = np.empty((len(frames), 4), np.float32)
            prev = None
            for i in range(len(frames)):
                q = Quaternion((float(v[i, 3]), float(v[i, 0]), float(v[i, 1]), float(v[i, 2])))
                if q.magnitude < 1e-6:
                    q = Quaternion()
                q.normalize()
                b = fixq_inv @ (rest_q_inv @ q) @ fixq
                if prev is not None and prev.dot(b) < 0:
                    b.negate()
                prev = b
                out[i] = (b.w, b.x, b.y, b.z)
            for k in range(4):
                _fill(make(base + ".rotation_quaternion", k, bone.name), frames + frame_start, out[:, k])
        pos = tr.get("pos")
        if pos is not None:
            frames, v = pos
            animated = bool(np.abs(v).max() > 1e-3)     # unanimated tracks hold (denormal) zeros
            if is_root:
                if not root_motion:
                    frames, v = frames[:1], np.zeros((1, 3), np.float32)
                p_game = [rest_p + Vector(x) * scale for x in v.tolist()]
            elif animated:
                p_game = [Vector(x) * scale for x in v.tolist()]
            elif first is not None:
                frames = frames[:1]
                p_game = [Vector(first[0].tolist()) * scale]
            else:
                p_game = []
            if p_game:
                out = np.empty((len(p_game), 3), np.float32)
                for i, p in enumerate(p_game):
                    d = rest_q_inv @ (p - rest_p)
                    out[i] = fixq_inv @ d
                if np.abs(out).max() > 1e-7 or len(out) > 1:
                    for k in range(3):
                        _fill(make(base + ".location", k, bone.name), frames + frame_start, out[:, k])
        sc = tr.get("scale")
        if sc is not None:
            frames, v = sc
            if np.abs(v - 1.0).max() > 1e-3:
                # game x = Blender bone y
                order = (1, 0, 2) if armature.get("xmd_bone_axis_fix", True) else (0, 1, 2)
                for k in range(3):
                    _fill(make(base + ".scale", k, bone.name), frames + frame_start, v[:, order[k]])
    action["xmd_clip"] = "%08X" % clip.name_hash
    action["xmd_fps"] = clip.fps
    action["xmd_frames"] = clip.frames
    action["xmd_matched_bones"] = matched
    try:
        action.use_fake_user = True
    except AttributeError:
        pass
    return action


def assign_action(armature, action):
    for pb in armature.pose.bones:
        pb.rotation_mode = "QUATERNION"
    if armature.animation_data is None:
        armature.animation_data_create()
    armature.animation_data.action = action
    if hasattr(action, "slots") and len(action.slots):
        try:
            armature.animation_data.action_slot = action.slots[0]
        except Exception:
            pass


def import_clips(ctx, armature, path, indices=None, root_motion=True, limit=None):
    """Import clips of an archive onto an armature.  Returns the list of actions."""
    from . import anim as an
    archive = ctx.game.open(path)
    base = os.path.basename(path).split(".")[0]
    infos = an.list_clips(archive)
    actions = []
    for i, info in enumerate(infos):
        if indices is not None and i not in indices:
            continue
        if limit is not None and len(actions) >= limit:
            break
        clip = an.read_clip(info)
        if clip is None:
            ctx.note("clip %d of %s could not be read" % (i, base))
            continue
        act = import_clip(ctx, armature, clip, "%s_%03d_%08X" % (base, i, clip.name_hash), root_motion)
        if act is not None:
            actions.append(act)
    return actions


# ------------------------------------------------------------------ levels

def import_level(ctx, path, unplaced=False):
    """Import one level archive: its static models, placed where its instance table says.
    Returns the list of objects made."""
    archive = ctx.game.open(path)
    base = os.path.basename(path).split(".")[0]
    ctx.texture_sources = [archive]
    for p in ctx.game.texture_archives(path):
        ctx.texture_sources.append(ctx.game.open(p))
    models = {m.name: m for m in md.read_models(archive) if m.vertex_count}
    mats = md.read_materials(archive)
    skeletons = md.read_skeletons(archive)
    instances = md.read_instances(archive)
    coll = bpy.data.collections.new(base)
    bpy.context.scene.collection.children.link(coll)
    ctx.collection = coll
    made = {}
    objects = []
    placed = set()
    missing = 0
    for inst in instances:
        m = models.get(inst.model)
        if m is None:
            missing += 1
            continue
        placed.add(inst.model)
        first = made.get(inst.model)
        if first is None:
            obj = build_mesh(ctx, archive, m, mats, "%s_%08X" % (base, m.name))
            if obj is None:
                continue
            made[inst.model] = obj
        else:
            obj = bpy.data.objects.new(first.name, first.data)
            coll.objects.link(obj)
            obj["xmd_model"] = first["xmd_model"]
        obj["xmd_instance"] = "%08X" % inst.id
        q = quat(inst.rot)
        if q.magnitude < 1e-6:
            q = Quaternion()
        obj.matrix_world = Matrix.LocRotScale(Vector(inst.pos) * ctx.scale, q.normalized(), Vector(inst.scale))
        objects.append(obj)
    if missing:
        ctx.note("%s: %d placed objects use models kept in other archives" % (base, missing))
    rest = [m for n, m in models.items() if n not in placed]
    if rest and (unplaced or not instances):
        sub = coll
        if instances:
            sub = bpy.data.collections.new(base + "_unplaced")
            coll.children.link(sub)
            ctx.collection = sub
        for m in rest:
            sk = skeletons.get(m.skeleton_id)
            arm = bone_names = None
            if sk is not None and md.VF_UBYTE4 in m.streams:
                arm, bone_names = build_armature(ctx, sk, "%s_%08X_skeleton" % (base, m.name))
                bpy.context.scene.collection.objects.unlink(arm)
                sub.objects.link(arm)
            obj = build_mesh(ctx, archive, m, mats, "%s_%08X" % (base, m.name), sk, arm, bone_names)
            if obj is not None:
                objects.append(obj)
    elif rest:
        ctx.note("%s: %d models without a placement were skipped" % (base, len(rest)))
    ctx.collection = None
    return objects
