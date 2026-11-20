"""Reads the game state an F4 capture wrote (capture_<stamp>\\state\\, src\\state_capture.cpp).

    python state.py <capture or state folder> summary
    python state.py <capture> read <address> [bytes=256]     hex dump of guest memory (big-endian words)
    python state.py <capture> u32 <address> [count=1]        32-bit words
    python state.py <capture> f32 <address> [count=1]        floats
    python state.py <capture> str <address>                  the zero-ended text there
    python state.py <capture> find <text | hex:0011AABB>     every place it is in memory (first 50)
    python state.py <capture> threads [name]                 the threads (registers, where each one is)
    python state.py <capture> lua [pattern]                  the script state's variables (lines with pattern)
    python state.py <capture> objects [pattern]              kernel objects: files open, events, threads ...

Addresses are the console's (0x8366855C, 8366855C). Memory left out of the dump reads as zero.
"""
import bisect
import os
import re
import struct
import sys


class State:
    def __init__(self, folder):
        if os.path.isdir(os.path.join(folder, "state")):
            folder = os.path.join(folder, "state")
        self.folder = folder
        self.runs = []      # (start, end, offset)
        self.regions = []   # (start, size, protect, heap)
        with open(os.path.join(folder, "memory_map.txt")) as f:
            for line in f:
                parts = line.split()
                if not parts or parts[0].startswith("#"):
                    continue
                if parts[0] == "region":
                    self.regions.append(tuple(int(p, 16) for p in parts[1:5]))
                else:
                    self.runs.append((int(parts[0], 16), int(parts[1], 16), int(parts[2], 16)))
        self.runs.sort()
        self.starts = [r[0] for r in self.runs]
        self.mem = open(os.path.join(folder, "memory.bin"), "rb")

    def read(self, address, size):
        out = bytearray(size)
        at = address
        while at < address + size:
            i = bisect.bisect_right(self.starts, at) - 1
            if i >= 0 and self.runs[i][0] <= at < self.runs[i][1]:
                start, end, offset = self.runs[i]
                n = min(end, address + size) - at
                self.mem.seek(offset + at - start)
                out[at - address:at - address + n] = self.mem.read(n)
                at += n
            else:
                nxt = self.starts[i + 1] if i + 1 < len(self.starts) else address + size
                at = min(max(nxt, at + 1), address + size)
        return bytes(out)

    def committed(self, address):
        return any(s <= address < s + n for s, n, _, _ in self.regions)

    def find(self, needle, limit=50):
        hits = []
        for start, end, offset in self.runs:
            self.mem.seek(offset)
            data = self.mem.read(end - start)
            at = data.find(needle)
            while at >= 0 and len(hits) < limit:
                hits.append(start + at)
                at = data.find(needle, at + 1)
            if len(hits) >= limit:
                break
        return hits


def number(text):
    return int(text, 16) if not text.isdigit() or text.startswith("0x") or len(text) == 8 else int(text)


def text_file(state, name, pattern):
    path = os.path.join(state.folder, name)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not pattern or re.search(pattern, line, re.I):
                sys.stdout.write(line)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    st = State(sys.argv[1])
    verb, args = sys.argv[2], sys.argv[3:]
    if verb == "summary":
        total = sum(e - s for s, e, _ in st.runs)
        committed = sum(n for _, n, _, _ in st.regions)
        print(f"{total / 2**20:.0f} MB stored in {len(st.runs)} ranges; {committed / 2**20:.0f} MB committed in "
              f"{len(st.regions)} regions")
        by_heap = {}
        for _, n, _, heap in st.regions:
            by_heap[heap] = by_heap.get(heap, 0) + n
        for heap, n in sorted(by_heap.items()):
            print(f"  heap {heap:08X}: {n / 2**20:.1f} MB committed")
        with open(os.path.join(st.folder, "threads.txt"), encoding="utf-8", errors="replace") as f:
            print(sum(1 for line in f if line.startswith("===")), "threads (threads.txt)")
    elif verb == "read":
        address, size = number(args[0]), int(args[1]) if len(args) > 1 else 256
        data = st.read(address, size)
        for row in range(0, size, 16):
            chunk = data[row:row + 16]
            words = " ".join(chunk[i:i + 4].hex().upper() for i in range(0, len(chunk), 4))
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"{address + row:08X}  {words:<36} {text}")
    elif verb in ("u32", "f32"):
        address, count = number(args[0]), int(args[1]) if len(args) > 1 else 1
        data = st.read(address, 4 * count)
        for i in range(count):
            (v,) = struct.unpack(">I" if verb == "u32" else ">f", data[4 * i:4 * i + 4])
            print(f"{address + 4 * i:08X}  " + (f"{v:08X} ({v})" if verb == "u32" else f"{v:g}"))
    elif verb == "str":
        data = st.read(number(args[0]), 4096)
        print(data.split(b"\0", 1)[0].decode("latin-1"))
    elif verb == "find":
        needle = bytes.fromhex(args[0][4:]) if args[0].startswith("hex:") else " ".join(args).encode()
        for hit in st.find(needle):
            print(f"{hit:08X}")
    elif verb == "threads":
        name = args[0] if args else None
        with open(os.path.join(st.folder, "threads.txt"), encoding="utf-8", errors="replace") as f:
            blocks = f.read().split("\n\n")
        for b in blocks:
            if b.strip() and (not name or re.search(name, b.splitlines()[0], re.I)):
                print(b + "\n")
    elif verb == "lua":
        text_file(st, "lua_globals.txt", args[0] if args else None)
    elif verb == "objects":
        text_file(st, "objects.txt", args[0] if args else None)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
