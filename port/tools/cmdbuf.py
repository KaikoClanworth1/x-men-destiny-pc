"""What is in the game's recorded D3D command buffers (dumped by the native
renderer to <game>/cmdbuf/<object>.bin: the 0xC0-byte object, then its memory).

    python cmdbuf.py <file or folder> [summary|list [n]|fixups]
"""
import collections
import glob
import os
import struct
import sys

OPS = {0x10: "NOP", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE",
       0x2D: "SET_CONSTANT", 0x2E: "SET_CONSTANT2", 0x2F: "LOAD_ALU_CONSTANT", 0x32: "SET_SHADER_CONSTANTS",
       0x3C: "WAIT_REG_MEM", 0x45: "REG_RMW", 0x46: "COND_WRITE", 0x48: "EVENT_WRITE", 0x4B: "EVENT_WRITE_SHD",
       0x4D: "EVENT_WRITE_EXT", 0x55: "INVALIDATE_STATE", 0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI",
       0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI", 0x3F: "INDIRECT_BUFFER", 0x50: "VIZ_QUERY",
       0x59: "CONTEXT_UPDATE", 0x5C: "ME_INIT", 0x3D: "INDIRECT_BUFFER_PFD"}


def packets(words, end):
    i = 0
    while i < end:
        h = words[i]
        t = h >> 30
        if t == 0:
            n = ((h >> 16) & 0x3FFF) + 1
            yield i, "reg", h & 0x7FFF, words[i + 1:i + 1 + n], (h >> 15) & 1
            i += 1 + n
        elif t == 1:
            yield i, "reg2", (h & 0x7FF, (h >> 11) & 0x7FF), words[i + 1:i + 3], 0
            i += 3
        elif t == 2:
            yield i, "nop", 0, [], 0
            i += 1
        else:
            n = ((h >> 16) & 0x3FFF) + 1
            yield i, "op", (h >> 8) & 0x7F, words[i + 1:i + 1 + n], h & 1
            i += 1 + n


def load(path):
    raw = open(path, "rb").read()
    obj = struct.unpack(">48I", raw[:0xC0])
    data = raw[0xC0:]
    base = obj[0x98 // 4]
    fix0, fix1 = obj[0x88 // 4], obj[0x8C // 4]
    words = struct.unpack(">%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
    end = ((fix0 & 0x1FFFFFFF) - (base & 0x1FFFFFFF)) // 4
    fixups = words[end:end + ((fix1 & 0x1FFFFFFF) - (fix0 & 0x1FFFFFFF)) // 4]
    return obj, words, end, fixups


def main():
    target = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "summary"
    files = sorted(glob.glob(os.path.join(target, "*.bin"))) if os.path.isdir(target) else [target]
    for path in files:
        obj, words, end, fixups = load(path)
        if mode == "summary":
            ops = collections.Counter()
            regs = collections.Counter()
            const_types = collections.Counter()
            for at, kind, what, args, pred in packets(words, end):
                if kind == "op":
                    ops[OPS.get(what, "%02X" % what)] += 1
                    if what in (0x2D, 0x2E) and args:
                        const_types[(args[0] >> 16) & 0xFF] += 1
                elif kind == "reg":
                    regs["%04X" % what] += 1
                else:
                    ops[kind] += 1
            print(os.path.basename(path), "words", end, "fixups", len(fixups), dict(ops))
            print("   set_constant types", dict(const_types), " reg writes", dict(regs.most_common(12)))
        elif mode == "list":
            limit = int(sys.argv[3]) if len(sys.argv) > 3 else 200
            for n, (at, kind, what, args, pred) in enumerate(packets(words, end)):
                if n >= limit:
                    break
                name = OPS.get(what, "%02X" % what) if kind == "op" else ("%04X" % what if kind == "reg" else str(what))
                print("%06X %s %s%s: %s" % (at * 4, kind, name, " P" if pred else "",
                                            " ".join("%08X" % a for a in args[:20]) + (" ..." if len(args) > 20 else "")))
        elif mode == "fixups":
            print(os.path.basename(path), " ".join("%08X" % o for o in obj))
            for f in fixups[:int(sys.argv[3]) if len(sys.argv) > 3 else 60]:
                print("  %08X: top %02X offset %06X index %d -> %08X" % (f, f >> 26, ((f >> 6) & 0xFFFFF) * 4, f & 0x3F,
                                                                      words[(f >> 6) & 0xFFFFF] if ((f >> 6) & 0xFFFFF) < len(words) else 0))


if __name__ == "__main__":
    main()
