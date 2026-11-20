"""Find the game's shaders on the disc: Xbox 360 XDK shader containers inside
the game's files (gzip-packed archives are unpacked in memory first).

    python find_shaders.py <game folder> [--extract <out dir>]

A container starts with the big-endian word 0x102A11xx (xx: 00 pixel, 01
vertex), then its two sizes. Prints per file how many it holds; with --extract
writes each distinct one as <fnv1a-64 of the container>.<vs|ps>.xsc.
"""
import gzip
import os
import struct
import sys


def fnv1a(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def scan(data):
    hits = []
    n = len(data)
    i = data.find(b"\x10\x2A\x11")
    while i != -1:
        if i + 0x24 <= n:
            flags, vsize, psize = struct.unpack_from(">III", data, i)
            size = vsize + psize
            if (flags & 0xFF) in (0, 1) and 0x24 < size <= n - i and size < 0x100000 and vsize >= 0x24:
                hits.append((i, size, flags))
        i = data.find(b"\x10\x2A\x11", i + 1)
    return hits


def main():
    folder = sys.argv[1]
    out = sys.argv[sys.argv.index("--extract") + 1] if "--extract" in sys.argv else None
    if out:
        os.makedirs(out, exist_ok=True)
    seen = {}
    aligned = 0
    for root, _, files in os.walk(folder):
        for name in sorted(files):
            path = os.path.join(root, name)
            if name.lower().endswith((".exe", ".dll", ".pdb", ".usm", ".xma", ".log")):
                continue
            try:
                if name.lower().endswith(".gz"):
                    with gzip.open(path, "rb") as f:
                        data = f.read()
                else:
                    with open(path, "rb") as f:
                        data = f.read()
            except OSError as e:
                print(f"{name}: {e}")
                continue
            hits = scan(data)
            if not hits:
                continue
            new = 0
            for at, size, flags in hits:
                blob = data[at:at + size]
                key = fnv1a(blob)
                aligned += at % 4 == 0
                if key in seen:
                    continue
                seen[key] = (name, at, size, flags)
                new += 1
                if out:
                    kind = "vs" if flags & 1 else "ps"
                    with open(os.path.join(out, f"{key:016X}.{kind}.xsc"), "wb") as f:
                        f.write(blob)
            print(f"{name}: {len(hits)} containers, {new} new")
    vs = sum(1 for v in seen.values() if v[3] & 1)
    print(f"{len(seen)} distinct shaders: {vs} vertex, {len(seen) - vs} pixel ({aligned} hits on 4-byte boundaries)")


if __name__ == "__main__":
    main()
