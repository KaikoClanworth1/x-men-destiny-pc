"""Compare one frame as the emulated GPU was given it (runs/gpu_draws.txt, made
by creating runs/gpu_dump_now.txt in an emulated test) with the same frame as
the native renderer drew it (runs/native_draws.txt, runs/dump_now.txt in a
native test): the first place the two differ is where the native renderer is
wrong.

    python frame_diff.py                 draws matched in order; what differs, by register / constant
    python frame_diff.py draw <native n> one matched pair in full detail
    python frame_diff.py list            the two draw sequences side by side (prim, count)

Draws are matched by their order and (primitive, index count); the two dumps
are of different runs, so moving things (particles, the HUD) don't line up.
"""
import collections
import difflib
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GROUPS = ["regs 2000", "regs 2100", "regs 2180", "regs 2200", "regs 2280", "regs 2300", "regs 2380", "bools",
          "fetch", "const"]
FIRST = {"regs 2000": 0x2000, "regs 2100": 0x2100, "regs 2180": 0x2180, "regs 2200": 0x2200, "regs 2280": 0x2280,
         "regs 2300": 0x2300, "regs 2380": 0x2380, "bools": 0x4900, "fetch": 0x4800, "const": 0x4000}
# registers the native side does not keep (or keeps elsewhere): not compared
SKIP = set(range(0x2003, 0x2011)) | {0x2011, 0x2012, 0x2100, 0x2101, 0x2103} | set(range(0x2280, 0x2295)) | \
    set(range(0x2300, 0x2326)) | {0x2182, 0x2183, 0x2184}


def load(path):
    draws = []
    with open(path, encoding="latin-1") as f:
        cur = None
        for line in f:
            if line.startswith("=== draw"):
                parts = line.split()
                prim = int(parts[parts.index("prim") + 1])
                count = int(parts[parts.index("count") + 1])
                cur = {"head": line.strip(), "prim": prim, "count": count}
                draws.append(cur)
            elif cur is not None and ":" in line:
                name, rest = line.split(":", 1)
                if name in FIRST:
                    cur[name] = [int(w, 16) for w in rest.split()]
    return draws


def flt(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def same(reg, a, b):
    if a == b:
        return True
    if 0x4000 <= reg < 0x4800:  # floats: nearly equal will do (another run, another frame)
        fa, fb = flt(a), flt(b)
        if fa != fa or fb != fb:
            return False
        return abs(fa - fb) <= 1e-3 * max(1.0, abs(fa), abs(fb))
    return False


def differences(g, n):
    out = []
    for name in GROUPS:
        a, b = g.get(name), n.get(name)
        if not a or not b:
            continue
        for k in range(min(len(a), len(b))):
            reg = FIRST[name] + k
            if reg in SKIP:
                continue
            if not same(reg, a[k], b[k]):
                out.append((reg, a[k], b[k]))
    return out


def name_of(reg):
    if 0x4000 <= reg < 0x4400:
        return "vertex c%d.%s" % ((reg - 0x4000) // 4, "xyzw"[reg & 3])
    if 0x4400 <= reg < 0x4800:
        return "pixel c%d.%s" % ((reg - 0x4400) // 4, "xyzw"[reg & 3])
    if 0x4800 <= reg < 0x48C0:
        slot = (reg - 0x4800) // 6
        return "fetch %d word %d (stream %d word %d)" % (slot, (reg - 0x4800) % 6, 95 - (reg - 0x4800) // 2, reg & 1)
    return "reg %04X" % reg


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "summary"
    gpu = load(os.path.join(ROOT, "runs", "gpu_draws.txt"))
    nat = load(os.path.join(ROOT, "runs", "native_draws.txt"))
    print("emulated: %d draws, native: %d draws" % (len(gpu), len(nat)))
    a = [(d["prim"], d["count"]) for d in gpu]
    b = [(d["prim"], d["count"]) for d in nat]
    matcher = difflib.SequenceMatcher(None, a, b, autojunk=False)
    pairs = []
    for i, j, n in matcher.get_matching_blocks():
        pairs.extend((i + k, j + k) for k in range(n))
    print("matched in order: %d" % len(pairs))
    if mode == "list":
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag != "equal":
                print(tag, "emulated %d-%d" % (i1, i2), a[i1:i1 + 6], "native %d-%d" % (j1, j2), b[j1:j1 + 6])
        return
    if mode == "draw":
        want = int(sys.argv[2])
        for i, j in pairs:
            if j == want:
                print("emulated:", gpu[i]["head"])
                print("native:  ", nat[j]["head"])
                for reg, x, y in differences(gpu[i], nat[j]):
                    extra = "  (%g / %g)" % (flt(x), flt(y)) if 0x4000 <= reg < 0x4800 else ""
                    print("  %-34s emulated %08X native %08X%s" % (name_of(reg), x, y, extra))
                return
        print("native draw %d has no match" % want)
        return
    by_reg = collections.Counter()
    example = {}
    clean = 0
    first_bad = []
    for i, j in pairs:
        d = differences(gpu[i], nat[j])
        if not d:
            clean += 1
            continue
        if len(first_bad) < 12:
            first_bad.append((j, len(d)))
        for reg, x, y in d:
            key = name_of(reg).split(" word")[0] if 0x4800 <= reg < 0x48C0 else name_of(reg).split(".")[0]
            by_reg[key] += 1
            example.setdefault(key, (j, reg, x, y))
    print("draws with nothing different: %d of %d" % (clean, len(pairs)))
    print("first native draws with differences (draw, fields):", first_bad)
    for key, n in by_reg.most_common(40):
        j, reg, x, y = example[key]
        extra = " (%g / %g)" % (flt(x), flt(y)) if 0x4000 <= reg < 0x4800 else ""
        print("%6d x %-26s e.g. native draw %d: emulated %08X native %08X%s" % (n, key, j, x, y, extra))


if __name__ == "__main__":
    main()
