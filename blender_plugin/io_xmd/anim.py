# Animation clips.  By KaikoClanworth
import struct

import numpy as np

from . import archive as ar


def bpe_expand(data, expected=0):
    """Byte-pair-encoding expander (blocks of: pair table, 16-bit length, bytes)."""
    out = bytearray()
    p = 0
    n = len(data)
    while p < n and (not expected or len(out) < expected):
        left = list(range(256))
        right = [0] * 256
        c = data[p]
        p += 1
        i = 0
        while True:
            if c > 127:
                i += c - 127
                c = 0
            if i == 256:
                break
            for _ in range(c + 1):
                left[i] = data[p]
                p += 1
                if left[i] != i:
                    right[i] = data[p]
                    p += 1
                i += 1
            if i == 256:
                break
            c = data[p]
            p += 1
        size = (data[p] << 8) | data[p + 1]
        p += 2
        # expand every byte value once, then translate the block
        table = [None] * 256

        def expand(v, depth=0):
            t = table[v]
            if t is None:
                if left[v] == v or depth > 64:
                    t = bytes((v,))
                else:
                    t = expand(left[v], depth + 1) + expand(right[v], depth + 1)
                table[v] = t
            return t
        block = data[p:p + size]
        p += size
        out += b"".join(expand(v) for v in block)
    return bytes(out)


class ClipInfo:
    __slots__ = ("id", "name_hash", "data_name", "offset", "size", "unpacked", "raw", "archive")


class Clip:
    """A decoded clip.  tracks[bone_hash] = {"pos": (frames, values), "rot": ..., "scale": ...};
    frames are frame numbers (ints), values numpy arrays (n,3) or (n,4 as x y z w).
    `first` holds the clip's first pose per bone hash: (pos, rot, scale)."""
    __slots__ = ("name_hash", "fps", "frames", "bones", "tracks", "first", "root_delta", "times")


def list_clips(archive):
    out = []
    for c in archive.all(ar.C_ANIM):
        d = c.data
        size = c.struct_size or 0x30
        for i in range(max(1, c.count)):
            if (i + 1) * size > len(d):
                break
            r = struct.unpack_from(">12I", d, i * size)
            ci = ClipInfo()
            ci.id, ci.name_hash, ci.data_name, ci.offset, ci.size, ci.unpacked = r[0], r[6], r[7], r[8], r[9], r[10]
            ci.raw = r
            ci.archive = archive
            out.append(ci)
    return out


def read_clip(info):
    chunk = info.archive.get(ar.C_ANIM_DATA, info.data_name)
    if chunk is None:
        return None
    packed = chunk.data[info.offset:info.offset + info.size]
    return parse_clip(bpe_expand(packed, info.unpacked))


def _halves(words):
    return np.array(words, ">u2").view(">f2").astype(np.float32)


def parse_clip(raw):
    if len(raw) < 0x34:
        return None
    clip = Clip()
    clip.name_hash = struct.unpack_from(">I", raw, 0)[0]
    clip.times = struct.unpack_from(">2f", raw, 4)
    clip.fps, clip.frames, n = struct.unpack_from(">3I", raw, 0x0C)
    clip.root_delta = struct.unpack_from(">7f", raw, 0x18)
    if n > 4096 or 0x34 + n * 30 > len(raw):
        return None
    clip.bones = list(struct.unpack_from(">%dI" % n, raw, 0x34))
    o = 0x34 + n * 4
    first = np.frombuffer(raw, ">u2", n * 13, o).reshape(n, 13)
    fh = first.view(">f2").astype(np.float32)
    clip.first = {}
    for i, h in enumerate(clip.bones):
        clip.first[h] = (fh[i, 1:4], fh[i, 5:9], fh[i, 10:13])
    o += n * 26
    words = np.frombuffer(raw, ">u2", (len(raw) - o) // 2, o)
    vals = words.view(">f2")
    keys = {}
    frame = 0
    i = 0
    total = len(words)
    w = words.tolist()
    while i < total:
        tag = w[i]
        t = tag >> 12
        if t == 4:
            frame = w[i + 1] if i + 1 < total else frame
            i += 2
        elif t == 5:
            i += 2
        elif t == 2:
            if i + 5 > total:
                break
            keys.setdefault((2, tag & 0xFFF), []).append((frame, i + 1))
            i += 5
        elif t == 1 or t == 3:
            if i + 4 > total:
                break
            keys.setdefault((t, tag & 0xFFF), []).append((frame, i + 1))
            i += 4
        else:
            break
    clip.tracks = {}
    kinds = {1: ("pos", 3), 2: ("rot", 4), 3: ("scale", 3)}
    for (t, idx), lst in keys.items():
        if idx >= n:
            continue
        name, width = kinds[t]
        lst.sort(key=lambda k: k[0])
        frames = []
        starts = []
        for f, s in lst:
            if frames and frames[-1] == f:
                starts[-1] = s
            else:
                frames.append(f)
                starts.append(s)
        idxs = np.array(starts, np.int64)[:, None] + np.arange(width)[None, :]
        clip.tracks.setdefault(clip.bones[idx], {})[name] = (np.array(frames, np.int32),
                                                           vals[idxs].astype(np.float32))
    return clip
