"""Print the PowerPC listing of a recompiled function (the comments of the generated code).

    python fn.py <address inside or at a function, hex> [more ...]     e.g. fn.py 824921A8

Finds the function holding the address through xmd_init.h's list, then prints the `// asm`
comment lines of its DEFINE_REX_FUNC body with labels. String-looking lis/addi pairs are
resolved against runs\image.bin when it exists.
"""
import bisect, glob, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "generated", "default")
IMG = os.path.join(HERE, "..", "runs", "image.bin")

def index():
    cache = os.path.join(HERE, "..", "runs", "fn_index.txt")
    if os.path.exists(cache):
        return [l.split() for l in open(cache)]
    out = []
    for f in glob.glob(os.path.join(GEN, "xmd_recomp.*.cpp")):
        for n, line in enumerate(open(f, encoding="utf-8", errors="replace")):
            if line.startswith("DEFINE_REX_FUNC(sub_"):
                out.append((line[20:28], os.path.basename(f), str(n)))
    out.sort()
    open(cache, "w").write("\n".join(" ".join(x) for x in out))
    return out

def main():
    idx = index()
    addrs = [int(x[0], 16) for x in idx]
    img = open(IMG, "rb").read() if os.path.exists(IMG) else b""
    def text_at(a):
        o = a - 0x82000000
        if 0 <= o < len(img):
            s = img[o:o + 60].split(b"\0")[0]
            if len(s) >= 3 and all(32 <= c < 127 for c in s):
                return s.decode()
        return None
    for arg in sys.argv[1:]:
        a = int(arg.replace("sub_", ""), 16)
        i = bisect.bisect_right(addrs, a) - 1
        name, f, n = idx[i]
        print(f"=== sub_{name} ({f}:{int(n) + 1})")
        lis = {}
        with open(os.path.join(GEN, f), encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
        for line in lines[int(n) + 1:]:
            if line.startswith("}"):
                break
            s = line.strip()
            if s.startswith("loc_"):
                print(s)
            elif s.startswith("// "):
                asm = s[3:]
                note = ""
                m = re.match(r"lis (r\d+),(-?\d+)", asm)
                if m:
                    lis[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
                m = re.match(r"(addi|ori) (r\d+),(r\d+),(-?\d+)", asm)
                if m and m.group(3) in lis:
                    v = (lis[m.group(3)] + int(m.group(4))) & 0xFFFFFFFF
                    t = text_at(v)
                    note = f"   ; {v:08X}" + (f" \"{t}\"" if t else "")
                print("    " + asm + note)

if __name__ == "__main__":
    main()
