"""Find this game's Direct3D library by matching it against a game whose D3D
functions are already known (the SvR 2011 port: docs/d3d_map.csv).

    python d3d_match.py <known generated dir> <known d3d_map.csv> <this generated dir> <out.csv>
                        [known range lo hi (hex)]

Both games link the XDK's Direct3D statically, without symbols. Each function
of the recompiled sources is reduced to its instruction list with addresses
taken out (branch targets, lis/addi halves of addresses); identical lists are
the same library function. What is left is matched by similarity inside the
address range the exact matches give.
"""
import csv
import difflib
import glob
import hashlib
import os
import pickle
import re
import sys

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\) \{", re.M)
ASM_RE = re.compile(r"^\t// ([a-z][\w.+-]*)[ \t]*(.*)$", re.M)
ADDR_RE = re.compile(r"0x8[0-9a-f]{7}")


def normalize(op, args):
    if op in ("lis", "addis"):
        return op  # half of an address or constant
    if op.startswith("b") and ADDR_RE.search(args):
        return op + " " + ADDR_RE.sub("@", args)
    return op + " " + args


def load(folder):
    # (kept beside this game's runs, whichever game the sources are of)
    key = hashlib.md5(os.path.abspath(folder).lower().encode()).hexdigest()[:10]
    cache = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "runs", f"d3d_match_{key}.pkl"))
    if os.path.exists(cache):
        with open(cache, "rb") as f:
            return pickle.load(f)
    funcs = {}
    for path in sorted(glob.glob(os.path.join(folder, "*_recomp.*.cpp"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        marks = [(m.start(), m.group(1)) for m in FUNC_RE.finditer(text)]
        for i, (at, name) in enumerate(marks):
            end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
            insns = [normalize(op, args.strip()) for op, args in ASM_RE.findall(text[at:end])]
            funcs[int(name[4:], 16)] = insns
    try:
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "wb") as f:
            pickle.dump(funcs, f)
    except OSError:
        pass
    return funcs


def digest(insns):
    return hashlib.md5("\n".join(insns).encode()).hexdigest()


def main():
    known_dir, map_csv, this_dir, out_csv = sys.argv[1:5]
    lo = int(sys.argv[5], 16) if len(sys.argv) > 6 else 0x82914800
    hi = int(sys.argv[6], 16) if len(sys.argv) > 6 else 0x82933240
    names = {}
    with open(map_csv, newline="") as f:
        for row in csv.DictReader(f):
            names[int(row["address"], 16)] = (row["name"], row.get("ring_per_frame", ""))
    known = {a: i for a, i in load(known_dir).items() if lo <= a < hi}
    this = load(this_dir)
    print(f"known library: {len(known)} functions; this game: {len(this)} functions")

    by_hash = {}
    for a, insns in this.items():
        if len(insns) >= 6:
            by_hash.setdefault(digest(insns), []).append(a)

    result = {}  # known address -> (this address, how)
    for a, insns in sorted(known.items()):
        hits = by_hash.get(digest(insns), []) if len(insns) >= 6 else []
        if len(hits) == 1:
            result[a] = (hits[0], "exact")
    exact = sorted(v[0] for v in result.values())
    print(f"exact, unique: {len(result)}")
    if not exact:
        print("no exact matches: different library build")
        return
    # The library's range here: the dense part of the exact matches.
    mid = exact[len(exact) // 2]
    near = [x for x in exact if abs(x - mid) < 0x40000]
    r_lo, r_hi = min(near) - 0x4000, max(near) + 0x4000
    print(f"library here: about {min(near):08X}-{max(near):08X} ({len(near)} of the exact matches)")

    taken = {v[0] for v in result.values()}
    pool = {a: i for a, i in this.items() if r_lo <= a < r_hi and a not in taken}
    for a, insns in sorted(known.items()):
        if a in result or len(insns) < 4:
            continue
        best, best_ratio = None, 0.0
        ops = [x.split(" ")[0] for x in insns]
        for b, other in pool.items():
            if not (0.7 * len(insns) <= len(other) <= 1.4 * len(insns) + 2):
                continue
            sm = difflib.SequenceMatcher(None, insns, other, autojunk=False)
            if sm.quick_ratio() < max(best_ratio, 0.6):
                continue
            ratio = sm.ratio()
            if ratio > best_ratio:
                best, best_ratio = b, ratio
        if best is not None and best_ratio >= 0.75:
            result[a] = (best, f"similar {best_ratio:.2f}")
            del pool[best]

    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["known", "here", "match", "instructions", "name", "known_ring_per_frame"])
        for a in sorted(known):
            here, how = result.get(a, (0, "none"))
            name, per_frame = names.get(a, ("", ""))
            w.writerow([f"{a:08X}", f"{here:08X}" if here else "", how, len(known[a]), name, per_frame])
    named = [a for a in known if names.get(a, ("",))[0]]
    print(f"matched {len(result)} of {len(known)}; named functions matched: "
          f"{sum(1 for a in named if a in result)} of {len(named)}")
    for a in sorted(named):
        here, how = result.get(a, (0, "none"))
        print(f"  {a:08X} -> {here:08X} {how:13s} {names[a][0]}")


if __name__ == "__main__":
    main()
