"""List the tags and disassemble the ActionScript 2 of a Scaleform .gfx / .swf movie.

    python gfx_dis.py <movie.gfx> [out.txt]
    python gfx_dis.py --carve <archive.zzz or .zzz.gz> <out folder>   (writes every GFX movie found)

Written for the X-Men: Destiny port (menus are Scaleform 3 movies, Flash 9 container,
ActionScript 2 byte code).  Output: one line per tag, the display list of every sprite
(names, depths, matrices), edit texts (variable, font, initial text) and the actions.
"""
import gzip
import os
import re
import struct
import sys

TAGS = {0: "End", 1: "ShowFrame", 2: "DefineShape", 4: "PlaceObject", 5: "RemoveObject", 9: "SetBackgroundColor",
        10: "DefineFont", 11: "DefineText", 12: "DoAction", 13: "DefineFontInfo", 20: "DefineBitsLossless",
        22: "DefineShape2", 24: "Protect", 26: "PlaceObject2", 28: "RemoveObject2", 32: "DefineShape3",
        33: "DefineText2", 34: "DefineButton2", 36: "DefineBitsLossless2", 37: "DefineEditText",
        39: "DefineSprite", 43: "FrameLabel", 48: "DefineFont2", 56: "ExportAssets", 57: "ImportAssets",
        59: "DoInitAction", 62: "DefineFontInfo2", 64: "EnableDebugger2", 65: "ScriptLimits", 69: "FileAttributes",
        70: "PlaceObject3", 71: "ImportAssets2", 73: "DefineFontAlignZones", 74: "CSMTextSettings",
        75: "DefineFont3", 76: "SymbolClass", 77: "Metadata", 78: "DefineScalingGrid", 83: "DefineShape4",
        84: "DefineMorphShape2", 46: "DefineMorphShape", 88: "DefineFontName",
        1000: "GFX_ExporterInfo", 1001: "GFX_DefineExternalImage", 1002: "GFX_FontTextureInfo",
        1003: "GFX_DefineExternalGradientImage", 1004: "GFX_DefineGradientMap", 1005: "GFX_DefineCompactedFont",
        1006: "GFX_DefineExternalSound", 1007: "GFX_DefineExternalStreamSound", 1008: "GFX_DefineSubImage",
        1009: "GFX_DefineExternalImage2"}

OPS = {0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop", 0x08: "ToggleQuality", 0x09: "StopSounds",
       0x0A: "Add", 0x0B: "Subtract", 0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals", 0x0F: "Less", 0x10: "And",
       0x11: "Or", 0x12: "Not", 0x13: "StringEquals", 0x14: "StringLength", 0x15: "StringExtract", 0x17: "Pop",
       0x18: "ToInteger", 0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2", 0x21: "StringAdd",
       0x22: "GetProperty", 0x23: "SetProperty", 0x24: "CloneSprite", 0x25: "RemoveSprite", 0x26: "Trace",
       0x27: "StartDrag", 0x28: "EndDrag", 0x29: "StringLess", 0x2A: "Throw", 0x2B: "CastOp", 0x2C: "ImplementsOp",
       0x30: "RandomNumber", 0x31: "MBStringLength", 0x32: "CharToAscii", 0x33: "AsciiToChar", 0x34: "GetTime",
       0x35: "MBStringExtract", 0x36: "MBCharToAscii", 0x37: "MBAsciiToChar", 0x3A: "Delete", 0x3B: "Delete2",
       0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return", 0x3F: "Modulo", 0x40: "NewObject",
       0x41: "DefineLocal2", 0x42: "InitArray", 0x43: "InitObject", 0x44: "TypeOf", 0x45: "TargetPath",
       0x46: "Enumerate", 0x47: "Add2", 0x48: "Less2", 0x49: "Equals2", 0x4A: "ToNumber", 0x4B: "ToString",
       0x4C: "PushDuplicate", 0x4D: "StackSwap", 0x4E: "GetMember", 0x4F: "SetMember", 0x50: "Increment",
       0x51: "Decrement", 0x52: "CallMethod", 0x53: "NewMethod", 0x54: "InstanceOf", 0x55: "Enumerate2",
       0x60: "BitAnd", 0x61: "BitOr", 0x62: "BitXor", 0x63: "BitLShift", 0x64: "BitRShift", 0x65: "BitURShift",
       0x66: "StrictEquals", 0x67: "Greater", 0x68: "StringGreater", 0x69: "Extends",
       0x81: "GotoFrame", 0x83: "GetURL", 0x87: "StoreRegister", 0x88: "ConstantPool", 0x8A: "WaitForFrame",
       0x8B: "SetTarget", 0x8C: "GoToLabel", 0x8D: "WaitForFrame2", 0x8E: "DefineFunction2", 0x8F: "Try",
       0x94: "With", 0x96: "Push", 0x99: "Jump", 0x9A: "GetURL2", 0x9B: "DefineFunction", 0x9D: "If",
       0x9E: "Call", 0x9F: "GotoFrame2"}


def cstr(d, p):
    e = d.index(b"\0", p)
    return d[p:e].decode("latin-1"), e + 1


class Bits:
    def __init__(self, d, p):
        self.d, self.p, self.bit = d, p, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit, self.p = 0, self.p + 1
        return v

    def s(self, n):
        v = self.u(n)
        if n and v >> (n - 1):
            v -= 1 << n
        return v

    def align(self):
        if self.bit:
            self.bit, self.p = 0, self.p + 1
        return self.p


def rect(d, p):
    b = Bits(d, p)
    n = b.u(5)
    r = [b.s(n) for _ in range(4)]
    return r, b.align()


def matrix(d, p):
    b = Bits(d, p)
    sx = sy = 1.0
    r0 = r1 = 0.0
    if b.u(1):
        n = b.u(5)
        sx, sy = b.s(n) / 65536.0, b.s(n) / 65536.0
    if b.u(1):
        n = b.u(5)
        r0, r1 = b.s(n) / 65536.0, b.s(n) / 65536.0
    n = b.u(5)
    tx, ty = b.s(n), b.s(n)
    return (sx, sy, r0, r1, tx / 20.0, ty / 20.0), b.align()


def cxform_a(d, p):
    b = Bits(d, p)
    has_add, has_mul = b.u(1), b.u(1)
    n = b.u(4)
    mul = [b.s(n) for _ in range(4)] if has_mul else None
    add = [b.s(n) for _ in range(4)] if has_add else None
    return (mul, add), b.align()


def actions(d, p, end, out, ind, pool=None):
    """Disassemble d[p:end]."""
    pool = pool or []
    base = p
    while p < end:
        at = p
        op = d[p]
        p += 1
        ln = 0
        if op >= 0x80:
            ln = struct.unpack_from("<H", d, p)[0]
            p += 2
        body = d[p:p + ln]
        name = OPS.get(op, f"op{op:02X}")
        text = ""
        if op == 0:
            out.append(f"{ind}{at - base:5d}: End")
            continue
        if op == 0x88:
            n = struct.unpack_from("<H", body, 0)[0]
            q, pool = 2, []
            for _ in range(n):
                s, q = cstr(body, q)
                pool.append(s)
            text = f"({n}) " + " | ".join(pool)
        elif op == 0x96:
            q, vals = 0, []
            while q < ln:
                t = body[q]
                q += 1
                if t == 0:
                    s, q = cstr(body, q)
                    vals.append(repr(s))
                elif t == 1:
                    vals.append(repr(struct.unpack_from("<f", body, q)[0]))
                    q += 4
                elif t == 2:
                    vals.append("null")
                elif t == 3:
                    vals.append("undefined")
                elif t == 4:
                    vals.append(f"r{body[q]}")
                    q += 1
                elif t == 5:
                    vals.append("true" if body[q] else "false")
                    q += 1
                elif t == 6:
                    hi, lo = struct.unpack_from("<II", body, q)
                    vals.append(repr(struct.unpack("<d", struct.pack("<II", lo, hi))[0]))
                    q += 8
                elif t == 7:
                    vals.append(str(struct.unpack_from("<i", body, q)[0]))
                    q += 4
                elif t == 8:
                    i = body[q]
                    q += 1
                    vals.append(repr(pool[i]) if i < len(pool) else f"c{i}")
                elif t == 9:
                    i = struct.unpack_from("<H", body, q)[0]
                    q += 2
                    vals.append(repr(pool[i]) if i < len(pool) else f"c{i}")
                else:
                    vals.append(f"?{t}")
                    break
            text = ", ".join(vals)
        elif op in (0x99, 0x9D):
            off = struct.unpack_from("<h", body, 0)[0]
            text = f"-> {p + ln + off - base}"
        elif op == 0x87:
            text = f"r{body[0]}"
        elif op == 0x81:
            text = str(struct.unpack_from("<H", body, 0)[0])
        elif op in (0x8C, 0x8B):
            text = repr(cstr(body, 0)[0])
        elif op == 0x83:
            a, q = cstr(body, 0)
            b, q = cstr(body, q)
            text = f"{a!r} {b!r}"
        elif op == 0x9F:
            text = f"play={body[0] & 1}"
        elif op == 0x9B:
            fn, q = cstr(body, 0)
            n = struct.unpack_from("<H", body, q)[0]
            q += 2
            params = []
            for _ in range(n):
                s, q = cstr(body, q)
                params.append(s)
            size = struct.unpack_from("<H", body, q)[0]
            out.append(f"{ind}{at - base:5d}: function {fn}({', '.join(params)}) {{")
            p += ln
            actions(d, p, p + size, out, ind + "    ", pool)
            out.append(f"{ind}       }}")
            p += size
            continue
        elif op == 0x8E:
            fn, q = cstr(body, 0)
            n = struct.unpack_from("<H", body, q)[0]
            regs = body[q + 2]
            flags = struct.unpack_from("<H", body, q + 3)[0]
            q += 5
            params = []
            for _ in range(n):
                r = body[q]
                s, q = cstr(body, q + 1)
                params.append(f"{s}=r{r}" if r else s)
            size = struct.unpack_from("<H", body, q)[0]
            pre = []
            r = 1
            for bit, nm in ((0, "this"), (2, "arguments"), (4, "super"), (6, "_root"), (7, "_parent"), (8, "_global")):
                if flags & (1 << bit):
                    pre.append(f"r{r}={nm}")
                    r += 1
            out.append(f"{ind}{at - base:5d}: function2 {fn}({', '.join(params)}) [{' '.join(pre)}] {{")
            p += ln
            actions(d, p, p + size, out, ind + "    ", pool)
            out.append(f"{ind}       }}")
            p += size
            continue
        elif op == 0x94:
            text = f"size {struct.unpack_from('<H', body, 0)[0]}"
        elif ln:
            text = body.hex()
        out.append(f"{ind}{at - base:5d}: {name} {text}".rstrip())
        p += ln
    return p


def tags(d, p, end, out, ind, version):
    frame = 0
    while p < end:
        hdr = struct.unpack_from("<H", d, p)[0]
        code, ln = hdr >> 6, hdr & 0x3F
        p += 2
        if ln == 0x3F:
            ln = struct.unpack_from("<I", d, p)[0]
            p += 4
        body_at, body_end = p, p + ln
        name = TAGS.get(code, f"Tag{code}")
        try:
            if code == 1:
                out.append(f"{ind}-- frame {frame} ends")
                frame += 1
            elif code == 43:
                out.append(f"{ind}FrameLabel {cstr(d, p)[0]!r} (frame {frame})")
            elif code == 12:
                out.append(f"{ind}DoAction (frame {frame}) @0x{body_at:X} len {ln}")
                actions(d, p, body_end, out, ind + "    ")
            elif code == 59:
                out.append(f"{ind}DoInitAction sprite {struct.unpack_from('<H', d, p)[0]} @0x{body_at:X} len {ln}")
                actions(d, p + 2, body_end, out, ind + "    ")
            elif code == 39:
                sid, frames = struct.unpack_from("<HH", d, p)
                out.append(f"{ind}DefineSprite {sid} frames {frames} @0x{body_at:X} len {ln} {{")
                tags(d, p + 4, body_end, out, ind + "  ", version)
                out.append(f"{ind}}}")
            elif code in (26, 70):
                q = p
                flags = d[q]
                q += 1
                flags2 = 0
                if code == 70:
                    flags2 = d[q]
                    q += 1
                depth = struct.unpack_from("<H", d, q)[0]
                q += 2
                parts = [f"depth {depth}"]
                if code == 70 and (flags2 & 8 or (flags2 & 0x10 and flags & 2)):
                    s, q = cstr(d, q)
                    parts.append(f"class {s!r}")
                if flags & 1:
                    parts.append("move")
                if flags & 2:
                    parts.append(f"char {struct.unpack_from('<H', d, q)[0]}")
                    q += 2
                if flags & 4:
                    m, q = matrix(d, q)
                    parts.append("at %.2f,%.2f" % (m[4], m[5]) + ("" if m[:4] == (1.0, 1.0, 0.0, 0.0) else " scale %.3f,%.3f rot %.3f,%.3f" % m[:4]))
                if flags & 8:
                    c, q = cxform_a(d, q)
                    parts.append(f"cx mul{c[0]} add{c[1]}")
                if flags & 0x10:
                    parts.append(f"ratio {struct.unpack_from('<H', d, q)[0]}")
                    q += 2
                if flags & 0x20:
                    s, q = cstr(d, q)
                    parts.append(f"name {s!r}")
                if flags & 0x40:
                    parts.append(f"clipdepth {struct.unpack_from('<H', d, q)[0]}")
                    q += 2
                out.append(f"{ind}{name} @0x{body_at:X} len {ln}: " + " ".join(parts))
                if flags & 0x80 and code == 26:
                    q += 2
                    q += 4 if version >= 6 else 2
                    while True:
                        ev = struct.unpack_from("<I", d, q)[0] if version >= 6 else struct.unpack_from("<H", d, q)[0]
                        q += 4 if version >= 6 else 2
                        if not ev:
                            break
                        size = struct.unpack_from("<I", d, q)[0]
                        q += 4
                        a = q
                        if ev & 0x20000:
                            a += 1
                        out.append(f"{ind}  clip event 0x{ev:X}:")
                        actions(d, a, q + size, out, ind + "      ")
                        q += size
            elif code == 28:
                out.append(f"{ind}RemoveObject2 depth {struct.unpack_from('<H', d, p)[0]}")
            elif code == 37:
                cid = struct.unpack_from("<H", d, p)[0]
                r, q = rect(d, p + 2)
                f1, f2 = d[q], d[q + 1]
                q += 2
                parts = [f"id {cid} rect {[x / 20 for x in r]} flags {f1:02X}{f2:02X}"]
                if f1 & 1:
                    parts.append(f"font {struct.unpack_from('<H', d, q)[0]}")
                    q += 2
                if f2 & 0x80:
                    s, q = cstr(d, q)
                    parts.append(f"fontclass {s!r}")
                if f1 & 1:
                    parts.append(f"height {struct.unpack_from('<H', d, q)[0] / 20}")
                    q += 2
                if f1 & 4:
                    parts.append("color " + d[q:q + 4].hex())
                    q += 4
                if f1 & 2:
                    parts.append(f"maxlen {struct.unpack_from('<H', d, q)[0]}")
                    q += 2
                if f2 & 0x20:
                    al, lm, rm, indent, lead = struct.unpack_from("<BHHHh", d, q)
                    parts.append(f"align {al} margins {lm / 20},{rm / 20} leading {lead / 20}")
                    q += 9
                s, q = cstr(d, q)
                parts.append(f"var {s!r}")
                if f1 & 0x80:
                    s, q = cstr(d, q)
                    parts.append(f"text {s!r}")
                out.append(f"{ind}DefineEditText @0x{body_at:X} len {ln}: " + " ".join(parts))
            elif code == 56:
                n = struct.unpack_from("<H", d, p)[0]
                q = p + 2
                for _ in range(n):
                    cid = struct.unpack_from("<H", d, q)[0]
                    s, q = cstr(d, q + 2)
                    out.append(f"{ind}Export {cid} as {s!r}")
            elif code in (57, 71):
                url, q = cstr(d, p)
                if code == 71:
                    q += 2
                n = struct.unpack_from("<H", d, q)[0]
                q += 2
                names = []
                for _ in range(n):
                    cid = struct.unpack_from("<H", d, q)[0]
                    s, q = cstr(d, q + 2)
                    names.append(f"{cid}={s}")
                out.append(f"{ind}Import from {url!r}: {', '.join(names)}")
            elif code == 34:
                bid = struct.unpack_from("<H", d, p)[0]
                out.append(f"{ind}DefineButton2 {bid} @0x{body_at:X} len {ln}")
                aoff = struct.unpack_from("<H", d, p + 3)[0]
                if aoff:
                    q = p + 3 + aoff
                    while True:
                        size, cond = struct.unpack_from("<HH", d, q)
                        e = q + size if size else body_end
                        out.append(f"{ind}  button cond 0x{cond:X}:")
                        actions(d, q + 4, e, out, ind + "      ")
                        if not size:
                            break
                        q = e
            elif code in (1001, 1009):
                out.append(f"{ind}{name} @0x{body_at:X} len {ln}: " + "".join(chr(c) if 32 <= c < 127 else "." for c in d[p:body_end]))
            elif code in (48, 75, 1005):
                out.append(f"{ind}{name} id {struct.unpack_from('<H', d, p)[0]} @0x{body_at:X} len {ln}")
            elif code in (2, 22, 32, 83, 11, 33, 20, 36, 46, 84):
                out.append(f"{ind}{name} id {struct.unpack_from('<H', d, p)[0]} @0x{body_at:X} len {ln}")
            elif code == 0:
                pass
            else:
                out.append(f"{ind}{name} @0x{body_at:X} len {ln}: {d[p:min(body_end, p + 48)].hex()}")
        except Exception as e:  # keep going: a tag this tool reads wrongly must not hide the rest
            out.append(f"{ind}{name} @0x{body_at:X} len {ln}: PARSE ERROR {e!r}")
        p = body_end
        if code == 0:
            break
    return p


def dump(d):
    out = []
    sig, version, length = d[:3], d[3], struct.unpack_from("<I", d, 4)[0]
    r, p = rect(d, 8)
    rate, frames = struct.unpack_from("<HH", d, p)
    out.append(f"{sig.decode()} version {version} length {length} stage {[x / 20 for x in r]} rate {rate / 256} frames {frames} tags@0x{p + 4:X}")
    tags(d, p + 4, len(d), out, "", version)
    return out


def main():
    if sys.argv[1] == "--carve":
        src, folder = sys.argv[2], sys.argv[3]
        os.makedirs(folder, exist_ok=True)
        d = gzip.open(src, "rb").read() if src.lower().endswith(".gz") else open(src, "rb").read()
        base = os.path.basename(src).split(".")[0]
        for m in re.finditer(b"GFX[\x06-\x0A]", d):
            o = m.start()
            ln = struct.unpack_from("<I", d, o + 4)[0]
            if ln <= len(d) - o:
                name = os.path.join(folder, f"{base}_{o:X}.gfx")
                open(name, "wb").write(d[o:o + ln])
                print(f"{name}: offset 0x{o:X} length {ln}")
        return
    d = open(sys.argv[1], "rb").read()
    text = "\n".join(dump(d))
    if len(sys.argv) > 2:
        open(sys.argv[2], "w", encoding="utf-8").write(text + "\n")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


if __name__ == "__main__":
    main()
