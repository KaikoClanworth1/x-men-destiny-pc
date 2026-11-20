"""Find code that builds a given address (lis + addi/ori) in the game image dump.

    python xref.py <image.bin> <address or text> [...]

image.bin = dump of the loaded image from 0x82000000 (XMD_DUMP_IMAGE). A text argument is
looked up as a zero-terminated string first. Prints the address of each instruction pair and
the function start guess (previous mflr/stwu prologue is not searched: use the generated
sources, `grep -l "sub_<addr>"`, or the nearest lower sub_ in xmd_init.h).
"""
import re, struct, sys

BASE = 0x82000000
CODE = (0x82330000, 0x831984EC)

def main():
    d = open(sys.argv[1], "rb").read()
    targets = {}
    for a in sys.argv[2:]:
        try:
            targets[int(a, 16)] = a
        except ValueError:
            for m in re.finditer(re.escape(a.encode()) + b"\0", d):
                s = m.start()
                while s > 0 and d[s - 1] != 0:
                    s -= 1
                targets[BASE + s] = d[s:m.end() - 1].decode("latin-1")
    for t, n in sorted(targets.items()):
        print(f"target {t:08X} {n!r}")
    lo, hi = CODE[0] - BASE, min(CODE[1] - BASE, len(d))
    words = struct.unpack_from(f">{(hi - lo) // 4}I", d, lo)
    reg = {}
    for i, w in enumerate(words):
        op = w >> 26
        if op == 15 and ((w >> 16) & 31) == 0:  # lis
            reg[(w >> 21) & 31] = (i, (w & 0xFFFF) << 16)
        elif op in (14, 24):  # addi / ori
            ra = (w >> 16) & 31 if op == 14 else (w >> 21) & 31
            if ra in reg and i - reg[ra][0] < 24:
                imm = w & 0xFFFF
                v = reg[ra][1] + (imm - 0x10000 if op == 14 and imm & 0x8000 else imm)
                v &= 0xFFFFFFFF
                if v in targets:
                    print(f"  {CODE[0] + i * 4:08X} builds {v:08X} {targets[v]!r} (lis at {CODE[0] + reg[ra][0] * 4:08X})")
        elif op in (18, 19) and False:
            reg.clear()

if __name__ == "__main__":
    main()
