# Xbox 360 texture decoding (tiled, big-endian DXT1/3/5, DXN, raw) to RGBA.
# By KaikoClanworth
import struct

import numpy as np

from . import archive as ar

# low 6 bits of the D3D format word = Xenos texture format
F_8 = 0x02
F_1555 = 0x03
F_565 = 0x04
F_8888 = 0x06
F_8_8 = 0x0A        # two channels
F_DXT1 = 0x12
F_DXT3 = 0x13
F_DXT5 = 0x14
F_4444 = 0x0F
F_16_16_16_16_F = 0x1A
F_DXN = 0x31
F_DXT3A = 0x3A
F_DXT5A = 0x3B
F_CTX1 = 0x3C

_BLOCK = {F_DXT1: 8, F_DXT3: 16, F_DXT5: 16, F_DXN: 16, F_DXT3A: 8, F_DXT5A: 8, F_CTX1: 8}
_PIXEL = {F_8: 1, F_1555: 2, F_565: 2, F_8888: 4, F_8_8: 2, F_4444: 2, F_16_16_16_16_F: 8}


class TextureInfo:
    __slots__ = ("id", "name", "width", "height", "depth", "format_word", "format",
                 "mips", "size", "data_name", "base_size", "kind", "chunk", "tiled", "streamed")


def read_headers(archive):
    """All texture headers of an archive, keyed by texture name hash."""
    out = {}
    for c in archive.all(ar.C_TEXTURE) + archive.all(ar.C_STREAM_TEXTURE):
        d = c.data
        size = c.struct_size or 0x48
        for i in range(max(1, c.count)):
            r = struct.unpack_from(">18I", d, i * size)
            t = TextureInfo()
            t.id, t.name = r[0], r[2]
            t.width, t.height, t.depth = r[5], r[6], r[7]
            t.format_word = r[8]
            t.format = r[8] & 0x3F
            t.tiled = bool(r[8] & 0x100)
            t.mips, t.size, t.data_name = r[9], r[10], r[11]
            t.kind = r[14]
            t.base_size = r[17]
            t.chunk = c
            t.streamed = c.cls == ar.C_STREAM_TEXTURE
            if t.name not in out or t.width * t.height > out[t.name].width * out[t.name].height:
                out[t.name] = t
    return out


def _tiled_offsets(bw, bh, pitch_w, log_bpp):
    """Byte offset of every block (bh x bw) inside a tiled surface."""
    x = np.arange(bw, dtype=np.int64)[None, :]
    y = np.arange(bh, dtype=np.int64)[:, None]
    aligned = (pitch_w + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7)
            + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6))


def _colours565(c0, c1):
    def rgb(c):
        r = ((c >> 11) & 31).astype(np.uint16)
        g = ((c >> 5) & 63).astype(np.uint16)
        b = (c & 31).astype(np.uint16)
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)
    return rgb(c0), rgb(c1)


def _dxt_colour(blocks, dxt1):
    """blocks: (n, 4) uint16 little-endian words c0 c1 idx_lo idx_hi -> (n, 4, 4, 4) uint8."""
    c0 = blocks[:, 0]
    c1 = blocks[:, 1]
    p0, p1 = _colours565(c0, c1)
    n = len(blocks)
    pal = np.zeros((n, 4, 4), np.uint16)
    pal[:, 0, :3] = p0
    pal[:, 1, :3] = p1
    pal[:, :, 3] = 255
    four = (c0 > c1) | (not dxt1)
    four = four[:, None]
    pal[:, 2, :3] = np.where(four, (2 * p0 + p1 + 1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four, (p0 + 2 * p1 + 1) // 3, 0)
    if dxt1:
        pal[:, 3, 3] = np.where(four[:, 0], 255, 0)
    idx = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 16)
    sh = (np.arange(16, dtype=np.uint32) * 2)[None, :]
    sel = (idx[:, None] >> sh) & 3
    out = np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(4, 2), axis=1)
    return out.reshape(n, 4, 4, 4).astype(np.uint8)


def _dxt5_alpha(blocks):
    """blocks: (n, 4) uint16 LE words (a0 a1 + 48 bits of indices) -> (n, 4, 4) uint8."""
    a0 = (blocks[:, 0] & 255).astype(np.int32)
    a1 = (blocks[:, 0] >> 8).astype(np.int32)
    n = len(blocks)
    pal = np.zeros((n, 8), np.int32)
    pal[:, 0] = a0
    pal[:, 1] = a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7,
                                 ((5 - i) * a0 + i * a1) // 5 if i < 5 else (0 if i == 5 else 255))
    bits = (blocks[:, 1].astype(np.uint64) | (blocks[:, 2].astype(np.uint64) << np.uint64(16))
            | (blocks[:, 3].astype(np.uint64) << np.uint64(32)))
    sh = (np.arange(16, dtype=np.uint64) * np.uint64(3))[None, :]
    sel = ((bits[:, None] >> sh) & np.uint64(7)).astype(np.int64)
    return np.take_along_axis(pal, sel, axis=1).reshape(n, 4, 4).astype(np.uint8)


def _dxt3_alpha(blocks):
    bits = (blocks[:, 0].astype(np.uint64) | (blocks[:, 1].astype(np.uint64) << np.uint64(16))
            | (blocks[:, 2].astype(np.uint64) << np.uint64(32)) | (blocks[:, 3].astype(np.uint64) << np.uint64(48)))
    sh = (np.arange(16, dtype=np.uint64) * np.uint64(4))[None, :]
    a = ((bits[:, None] >> sh) & np.uint64(15)).astype(np.uint8)
    return (a * 17).reshape(len(blocks), 4, 4)


def _ctx1(blocks):
    """CTX1: two 8.8 end points + 2-bit indices -> (n,4,4,2)."""
    e0 = blocks[:, 0]
    e1 = blocks[:, 1]
    n = len(blocks)
    pal = np.zeros((n, 4, 2), np.int32)
    pal[:, 0, 0] = e0 & 255
    pal[:, 0, 1] = e0 >> 8
    pal[:, 1, 0] = e1 & 255
    pal[:, 1, 1] = e1 >> 8
    pal[:, 2] = (2 * pal[:, 0] + pal[:, 1] + 1) // 3
    pal[:, 3] = (pal[:, 0] + 2 * pal[:, 1] + 1) // 3
    idx = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 16)
    sh = (np.arange(16, dtype=np.uint32) * 2)[None, :]
    sel = ((idx[:, None] >> sh) & 3).astype(np.int64)
    out = np.take_along_axis(pal, sel[:, :, None].repeat(2, 2), axis=1)
    return out.reshape(n, 4, 4, 2).astype(np.uint8)


def decode_surface(raw, width, height, fmt, tiled=True, packed=False):
    """One mip level's bytes -> (height, width, 4) uint8 RGBA, top row first.
    Returns None for formats that are not handled."""
    if fmt in _BLOCK:
        bpb = _BLOCK[fmt]
        unit = 4
    elif fmt in _PIXEL:
        bpb = _PIXEL[fmt]
        unit = 1
    else:
        return None
    bw = max(1, (width + unit - 1) // unit)
    bh = max(1, (height + unit - 1) // unit)
    ox = oy = 0
    if packed and min(width, height) <= 16:
        # small textures with mips live inside the "mip tail" tile
        if width > height:
            oy = 16 // unit
        else:
            ox = 16 // unit
    log_bpp = bpb.bit_length() - 1
    pitch_w = (bw + ox + 31) & ~31
    pitch_h = (bh + oy + 31) & ~31
    buf = np.frombuffer(raw, np.uint8)
    if tiled:
        off = _tiled_offsets(bw + ox, bh + oy, pitch_w, log_bpp)[oy:, ox:]
    else:
        x = np.arange(bw, dtype=np.int64)[None, :] + ox
        y = np.arange(bh, dtype=np.int64)[:, None] + oy
        off = (y * pitch_w + x) * bpb
    need = int(off.max()) + bpb
    if need > len(buf):
        if not tiled or pitch_w * pitch_h * bpb > len(buf) + bpb * 32 * 32 * 4:
            return None
        buf = np.concatenate([buf, np.zeros(need - len(buf), np.uint8)])
    idx = off.reshape(-1, 1) + np.arange(bpb, dtype=np.int64)[None, :]
    blocks = buf[idx]                                   # (n, bpb) bytes in file order
    n = bw * bh
    if fmt in _BLOCK:
        w16 = (blocks[:, 0::2].astype(np.uint16) << 8) | blocks[:, 1::2]   # big-endian words
        if fmt == F_DXT1:
            px = _dxt_colour(w16, True)
        elif fmt == F_DXT3:
            px = _dxt_colour(w16[:, 4:], False)
            px[..., 3] = _dxt3_alpha(w16[:, :4])
        elif fmt == F_DXT5:
            px = _dxt_colour(w16[:, 4:], False)
            px[..., 3] = _dxt5_alpha(w16[:, :4])
        elif fmt == F_DXN:
            px = np.zeros((n, 4, 4, 4), np.uint8)
            px[..., 0] = _dxt5_alpha(w16[:, :4])
            px[..., 1] = _dxt5_alpha(w16[:, 4:])
            px[..., 2] = 255
            px[..., 3] = 255
        elif fmt == F_DXT5A:
            a = _dxt5_alpha(w16)
            px = np.stack([a, a, a, np.full_like(a, 255)], -1)
        elif fmt == F_DXT3A:
            a = _dxt3_alpha(w16)
            px = np.stack([a, a, a, np.full_like(a, 255)], -1)
        else:  # CTX1
            rg = _ctx1(w16)
            px = np.zeros((n, 4, 4, 4), np.uint8)
            px[..., :2] = rg
            px[..., 2] = 255
            px[..., 3] = 255
        img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
        return np.ascontiguousarray(img[:height, :width])
    b = blocks
    if fmt == F_8888:      # stored A R G B
        img = np.stack([b[:, 1], b[:, 2], b[:, 3], b[:, 0]], -1)
    elif fmt == F_8:
        img = np.stack([b[:, 0], b[:, 0], b[:, 0], np.full(n, 255, np.uint8)], -1)
    elif fmt == F_8_8:
        img = np.stack([b[:, 1], b[:, 0], np.full(n, 255, np.uint8), np.full(n, 255, np.uint8)], -1)
    elif fmt in (F_565, F_1555, F_4444):
        v = (b[:, 0].astype(np.uint16) << 8) | b[:, 1]
        if fmt == F_565:
            r, g, bl = (v >> 11) & 31, (v >> 5) & 63, v & 31
            img = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2),
                            np.full(n, 255, np.uint16)], -1)
        elif fmt == F_1555:
            r, g, bl = (v >> 10) & 31, (v >> 5) & 31, v & 31
            img = np.stack([(r << 3) | (r >> 2), (g << 3) | (g >> 2), (bl << 3) | (bl >> 2),
                            (v >> 15) * 255], -1)
        else:
            img = np.stack([((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17], -1)
        img = img.astype(np.uint8)
    elif fmt == F_16_16_16_16_F:
        f = np.frombuffer(b.tobytes(), ">f2").reshape(n, 4).astype(np.float32)
        img = (np.clip(f, 0, 1) * 255).astype(np.uint8)
    else:
        return None
    return np.ascontiguousarray(img.reshape(bh, bw, 4)[:height, :width])


def base_level_size(t):
    """Bytes of the largest mip as stored (aligned to 32x32 units, 4096 bytes)."""
    if t.format in _BLOCK:
        bpb, unit = _BLOCK[t.format], 4
    elif t.format in _PIXEL:
        bpb, unit = _PIXEL[t.format], 1
    else:
        return 0
    bw = ((max(1, (t.width + unit - 1) // unit)) + 31) & ~31
    bh = ((max(1, (t.height + unit - 1) // unit)) + 31) & ~31
    return (bw * bh * bpb + 4095) & ~4095


def decode(t, data, level_width=None, level_height=None):
    """Decode the largest mip found in `data` (the texture's data chunk bytes)."""
    w = level_width or t.width
    h = level_height or t.height
    return decode_surface(data, w, h, t.format, t.tiled, t.mips > 1)


def base_level_size_wh(t, w, h):
    if t.format in _BLOCK:
        bpb, unit = _BLOCK[t.format], 4
    elif t.format in _PIXEL:
        bpb, unit = _PIXEL[t.format], 1
    else:
        return 0
    bw = ((max(1, (w + unit - 1) // unit)) + 31) & ~31
    bh = ((max(1, (h + unit - 1) // unit)) + 31) & ~31
    return (bw * bh * bpb + 4095) & ~4095
