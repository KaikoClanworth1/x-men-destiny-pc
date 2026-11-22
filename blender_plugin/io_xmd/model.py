# Model, skeleton and material parsing (no Blender calls here).  By KaikoClanworth
import struct

import numpy as np

from . import archive as ar

# vertex stream formats (the "format" field of a stream description)
VF_FLOAT3 = 1        # guess for static meshes, see read_stream
VF_HALF4 = 3         # position
VF_FRAME = 4         # 3 x 4 signed bytes: tangent, binormal, normal
VF_UBYTE4 = 7        # bone indices into the section's palette
VF_UBYTE4N = 8       # bone weights
VF_COLOUR = 9
VF_HALF2 = 0xD       # texture coordinates
VF_STATIC = 0xE      # interleaved static vertex: float3 position, 10:11:11 normal, tangent,
                     # binormal, half2 uv, half2 lightmap uv (32 bytes)


class Section:
    __slots__ = ("flags", "index_start", "tri_count", "min_vertex", "max_vertex", "shader_set",
                 "material", "palette_count", "palette_offset", "bbox")


class Bone:
    __slots__ = ("name_hash", "parent", "rot", "pos", "scale")


class Skeleton:
    __slots__ = ("id", "bones", "palette", "palette_id")


class Material:
    __slots__ = ("id", "textures", "slots", "params", "raw")


class Model:
    __slots__ = ("name", "skeleton_id", "sections", "streams", "indices", "vertex_count", "raw", "static")


def _container(d):
    """Header shared by skeleton-like chunks: 44556677, version, size, 0."""
    if len(d) < 0x20 or d[:4] != b"\x44\x55\x66\x77":
        return None
    return struct.unpack_from(">4I", d, 0)


def read_skeleton(chunk):
    d = chunk.data
    if _container(d) is None:
        return None
    sid, count, rec, _ = struct.unpack_from(">4I", d, 0x10)
    if rec != 0x40 or 0x20 + count * rec > len(d):
        return None
    sk = Skeleton()
    sk.id = sid
    sk.bones = []
    for i in range(count):
        o = 0x20 + i * rec
        q = struct.unpack_from(">4f", d, o)
        p = struct.unpack_from(">3f", d, o + 0x10)
        name, parent = struct.unpack_from(">II", d, o + 0x20)
        b = Bone()
        b.rot, b.pos, b.name_hash = q, p, name
        b.parent = parent if parent != i and parent < count else -1
        b.scale = (1.0, 1.0, 1.0)
        sk.bones.append(b)
    o = 0x20 + count * rec
    sk.palette = []
    sk.palette_id = 0
    if o + 0x10 <= len(d):
        pid, pcount, psize, _ = struct.unpack_from(">4I", d, o)
        if psize == 2 and o + 0x10 + pcount * 2 <= len(d):
            sk.palette_id = pid
            sk.palette = list(struct.unpack_from(">%dH" % pcount, d, o + 0x10))
    return sk


def read_skeletons(archive):
    out = {}
    for c in archive.all(ar.C_SKELETON):
        sk = read_skeleton(c)
        if sk:
            out[sk.id] = sk
    return out


def read_sections(chunk):
    d = chunk.data
    out = []
    for i in range(len(d) // 0x4C):
        r = struct.unpack_from(">12I6fI", d, i * 0x4C)
        s = Section()
        s.flags, s.index_start, s.tri_count, s.min_vertex, s.max_vertex = r[1], r[2], r[3], r[4], r[5]
        s.shader_set, s.material, s.palette_count, s.palette_offset = r[6], r[7], r[8], r[9]
        s.bbox = r[12:18]
        out.append(s)
    return out


def read_materials(archive):
    """Material instances keyed by material id (the hash a section names)."""
    out = {}
    for c in archive.all(ar.C_MATERIAL):
        d = c.data
        size = c.struct_size or 0xB0
        for i in range(max(1, c.count)):
            if (i + 1) * size > len(d):
                break
            r = struct.unpack_from(">44I", d, i * size)
            m = Material()
            m.id = r[0]
            counts = r[43]
            ntex = (counts >> 16) & 0xFF
            m.textures = [x for x in r[7:7 + min(ntex, 16)]]
            m.slots = [x for x in r[23:23 + min(ntex, 16)]]
            m.raw = r
            m.params = {}
            pd = archive.get(ar.C_MATERIAL_DEF, m.id)
            if pd:
                b = pd.data
                n = struct.unpack_from(">I", b, 0)[0]
                q = 4
                for _ in range(n):
                    if q + 0x5C > len(b):
                        break
                    vals = struct.unpack_from(">4f", b, q + 8)
                    name = b[q + 24:q + 0x5C].split(b"\0")[0].decode("ascii", "replace")
                    m.params[name] = vals
                    q += 0x5C
            out.setdefault(m.id, m)
    return out


def read_stream(data, fmt, count):
    """Decode one vertex stream to a numpy array, or None when the format is unknown."""
    if count <= 0:
        return None
    stride = len(data) // count
    if fmt == VF_HALF4 and stride == 8:
        return np.frombuffer(data, ">f2", count * 4).reshape(count, 4).astype(np.float32)
    if fmt == VF_HALF2 and stride == 4:
        return np.frombuffer(data, ">f2", count * 2).reshape(count, 2).astype(np.float32)
    if fmt == VF_FRAME and stride == 12:
        b = np.frombuffer(data, np.int8, count * 12).reshape(count, 3, 4)
        return b[:, :, ::-1][:, :, :3].astype(np.float32) / 127.0     # x y z of each vector
    if fmt in (VF_UBYTE4, VF_UBYTE4N, VF_COLOUR) and stride == 4:
        return np.frombuffer(data, np.uint8, count * 4).reshape(count, 4)
    if stride == 12:
        return np.frombuffer(data, ">f4", count * 3).reshape(count, 3).astype(np.float32)
    return None


def _unpack_10_11_11(v):
    """Packed normal: x in the low 11 bits, y in the next 11, z in the top 10 (all signed)."""
    v = v.astype(np.int64)
    x = v & 0x7FF
    y = (v >> 11) & 0x7FF
    z = (v >> 22) & 0x3FF
    x = np.where(x >= 0x400, x - 0x800, x) / 1023.0
    y = np.where(y >= 0x400, y - 0x800, y) / 1023.0
    z = np.where(z >= 0x200, z - 0x400, z) / 511.0
    return np.stack([x, y, z], -1).astype(np.float32)


def read_static_stream(data, count, streams):
    """Interleaved static-mesh vertices -> entries "pos", "normal", "uv", "uv2" in `streams`."""
    if count <= 0:
        return
    stride = len(data) // count
    if stride < 12:
        return
    raw = np.frombuffer(data, np.uint8, count * stride).reshape(count, stride)
    streams["pos"] = np.ascontiguousarray(raw[:, 0:12]).view(">f4").reshape(count, 3).astype(np.float32)
    if stride >= 16:
        streams["normal"] = _unpack_10_11_11(np.ascontiguousarray(raw[:, 12:16]).view(">u4").reshape(count))
    if stride >= 28:
        streams["uv"] = np.ascontiguousarray(raw[:, 24:28]).view(">f2").reshape(count, 2).astype(np.float32)
    if stride >= 32:
        streams["uv2"] = np.ascontiguousarray(raw[:, 28:32]).view(">f2").reshape(count, 2).astype(np.float32)
    streams["stride"] = stride


def read_models(archive):
    """Every model of an archive: geometry arrays + sections."""
    stream_descs = {}       # data chunk name -> list of (usage, fmt, size, offset)
    for c in archive.all(ar.C_STREAMS):
        d = c.data
        for i in range(len(d) // 0x1C):
            sid, _, usage, fmt, size, off, dname = struct.unpack_from(">7I", d, i * 0x1C)
            stream_descs.setdefault(dname, []).append((usage, fmt, size, off))
    index_descs = {}
    for c in archive.all(ar.C_INDEX_DESC):
        d = c.data
        for i in range(len(d) // 0x1C):
            sid, _, usage, size, off, fmt, dname = struct.unpack_from(">7I", d, i * 0x1C)
            index_descs[dname] = (size, off, fmt)
    model_info = {}
    static_names = set()
    for c in archive.all(ar.C_STATIC_MODEL):
        size = c.struct_size or len(c.data)
        for i in range(max(1, c.count)):
            if (i + 1) * size <= len(c.data):
                static_names.add(struct.unpack_from(">I", c.data, i * size)[0])
    for c in archive.all(ar.C_MODEL) + archive.all(ar.C_STATIC_MODEL):
        d = c.data
        size = c.struct_size or len(d)
        for i in range(max(1, c.count)):
            if (i + 1) * size > len(d):
                break
            r = struct.unpack_from(">%dI" % (size // 4), d, i * size)
            model_info[r[0]] = r
    out = []
    for sc in archive.all(ar.C_SECTIONS):
        name = sc.name
        m = Model()
        m.name = name
        m.raw = model_info.get(name)
        m.skeleton_id = m.raw[4] if m.raw and len(m.raw) > 4 else 0
        m.static = archive.get(ar.C_MODEL, 0) is None and name in static_names
        m.sections = read_sections(sc)
        m.streams = {}
        m.indices = None
        m.vertex_count = 0
        vd = archive.get(ar.C_VERTEX_DATA, name)
        idc = archive.get(ar.C_INDEX_DATA, name)
        if vd is None or idc is None or not m.sections:
            out.append(m)
            continue
        count = max(s.max_vertex for s in m.sections) + 1
        m.vertex_count = count
        vdata = vd.data
        for usage, fmt, size, off in stream_descs.get(name, []):
            if fmt == VF_STATIC:
                read_static_stream(vdata[off:off + size], count, m.streams)
                continue
            arr = read_stream(vdata[off:off + size], fmt, count)
            if arr is not None:
                m.streams.setdefault(fmt, arr)
            m.streams[("usage", usage)] = (fmt, size, off)
        size, off, fmt = index_descs.get(name, (idc.size, 0, 2))
        raw = idc.data[off:off + size]
        m.indices = np.frombuffer(raw, ">u4" if fmt == 4 else ">u2").astype(np.int64)
        out.append(m)
    return out


class Instance:
    __slots__ = ("id", "model", "pos", "rot", "scale", "lightmap")


def read_instances(archive):
    """Placed objects of a level archive: model name hash + position, rotation (x y z w), scale."""
    out = []
    for c in archive.all(ar.C_INSTANCES):
        d = c.data
        size = c.struct_size or 0x14C
        if size < 0xB8:
            continue
        for i in range(len(d) // size):
            o = i * size
            inst = Instance()
            inst.id = struct.unpack_from(">I", d, o)[0]
            inst.scale = struct.unpack_from(">3f", d, o + 0x40)
            inst.pos = struct.unpack_from(">3f", d, o + 0x68)
            inst.rot = struct.unpack_from(">4f", d, o + 0x78)
            inst.model = struct.unpack_from(">I", d, o + 0xB4)[0]
            inst.lightmap = struct.unpack_from(">I", d, o + 0xE0)[0] if size >= 0xE4 else 0
            out.append(inst)
    return out
