"""Study the game's bound vertex shader objects (dumps of src/d3d_probe.cpp,
XMD_SHADER_OBJECTS) against the disc's containers (tools/find_shaders.py).

    python vs_objects.py <runs folder> <disc containers folder>

For each dump: the disc container with the same virtual part, which microcode
instructions differ from it (binding a vertex shader to its vertex format
rewrites its vertex fetch instructions) and the fetches decoded.
"""
import glob
import os
import struct
import sys

USAGE = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
         "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]


def elements(container):
    """(instruction address, usage, usage index) of a vertex shader container."""
    shader_at = struct.unpack_from(">I", container, 0x18)[0]
    field18, count = struct.unpack_from(">II", container, shader_at + 0x18)
    out = []
    for i in range(count):
        v = struct.unpack_from(">I", container, shader_at + 0x24 + 4 * (field18 + i))[0]
        out.append((v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF))
    return out


def fetch(words):
    """A vertex fetch instruction's fields (3 dwords, as the GPU reads them)."""
    w0, w1, w2 = words
    return {
        "opcode": w0 & 0x1F,
        "const": (w0 >> 20) & 0x1F, "const_sel": (w0 >> 25) & 3,
        "format": (w1 >> 16) & 0x3F, "signed": (w1 >> 12) & 1, "normalized": 1 - ((w1 >> 13) & 1),
        "swizzle": w1 & 0xFFF,
        "stride": w2 & 0xFF, "offset": (w2 >> 8) & 0x7FFFFF, "mini": (w1 >> 30) & 1,
    }


def main():
    runs, disc = sys.argv[1], sys.argv[2]
    by_virtual = {}
    for path in glob.glob(os.path.join(disc, "*.vs.xsc")):
        data = open(path, "rb").read()
        vsize = struct.unpack_from(">I", data, 4)[0]
        by_virtual.setdefault(data[:vsize], []).append(path)
    print(f"{sum(len(v) for v in by_virtual.values())} vertex containers on the disc, {len(by_virtual)} distinct virtual parts")
    found = other = 0
    for path in sorted(glob.glob(os.path.join(runs, "vsobj_*_*_*_*.bin"))):
        vsize, psize = (int(x, 16) for x in os.path.basename(path)[:-4].split("_")[3:5])
        data = open(path, "rb").read()
        virtual, micro = data[0x368:0x368 + vsize], data[0x368 + vsize:]
        # (a bound shader's copy has flag 0x40 set; otherwise the container's virtual part as on the disc)
        virtual = virtual[:3] + bytes([virtual[3] & 0xBF]) + virtual[4:]
        elems = elements(virtual)
        hits = by_virtual.get(virtual, [])
        note = "no disc container with this virtual part"
        if hits:
            found += 1
            ref = open(hits[0], "rb").read()[vsize:]
            n = min(len(ref), len(micro)) // 12
            diff = {i for i in range(n) if ref[12 * i:12 * i + 12] != micro[12 * i:12 * i + 12]}
            addresses = {a for a, _, _ in elems}
            note = (f"{os.path.basename(hits[0])[:16]} ({len(hits)} with it): {len(diff)} instructions differ, "
                    f"{len(diff - addresses)} of them not vertex fetches, {len(addresses - diff)} fetches unchanged")
        else:
            other += 1
        print(f"{os.path.basename(path)}: header {data[0x28:0x44].hex()} {note}")
        for a, usage, index in elems:
            f = fetch(struct.unpack_from(">3I", micro, 12 * a))
            print(f"    @{a:3d} {USAGE[usage]}{index}: op {f['opcode']} slot {f['const']}.{f['const_sel']} offset {f['offset']} "
                  f"stride {f['stride']} format {f['format']} signed {f['signed']} norm {f['normalized']} swizzle {f['swizzle']:03X}")
    print(f"{found} matched a disc container's virtual part, {other} did not")


if __name__ == "__main__":
    main()
