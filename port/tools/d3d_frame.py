"""Summarize a census frame log (runs/d3dtrace/frame_<n>.txt, src/d3d_trace.cpp).

    python d3d_frame.py <frame file> [summary | passes | calls <address> [n]]

summary   every function the game itself calls (not nested), with its count,
          the name d3d_match.py gave it and one call's arguments
passes    the frame in order, runs of the frequent calls collapsed - shows the
          render passes (set target, clear, draws, resolve)
calls     the first n calls of one function, with what they call inside
"""
import collections
import csv
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LINE = re.compile(r"^( *)([0-9A-F]{8}) (.*)$")


def names():
    out = {}
    path = os.path.join(ROOT, "runs", "d3d_match.csv")
    if os.path.exists(path):
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                if row["here"] and row["name"]:
                    out[row["here"]] = row["name"]
    out.update(EXTRA)
    return out


# Named from this game's own frame logs.
EXTRA = {
    "8272F6B8": "D3DDevice_SetPixelShaderConstantF",
    "8272F790": "D3DDevice_SetVertexShaderConstantF",
    "82738208": "D3DDevice_Present (Swap)",
}


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if m:
                rows.append((len(m.group(1)) // 2, m.group(2), m.group(3)))
    return rows


def main():
    rows = load(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "summary"
    name = names()
    top = [r for r in rows if r[0] == 0]
    count = collections.Counter(r[1] for r in top)
    if mode == "summary":
        first = {}
        for r in top:
            first.setdefault(r[1], r[2])
        print(f"{len(rows)} calls, {len(top)} from the game, {len(count)} functions")
        for a, n in sorted(count.items(), key=lambda x: -x[1]):
            print(f"{a} {n:6d}  {name.get(a, ''):45s} {first[a][:96]}")
    elif mode == "passes":
        often = {a for a, n in count.items() if n > 60}
        run = collections.Counter()

        def flush():
            if run:
                print("    ... " + ", ".join(f"{name.get(a, a)[:28]} x{n}" for a, n in run.most_common(6)))
                run.clear()
        for _, a, rest in top:
            if a in often:
                run[a] += 1
                continue
            flush()
            print(f"{a} {name.get(a, ''):34s} {rest[:110]}")
        flush()
    elif mode == "calls":
        want, limit = sys.argv[3].upper(), int(sys.argv[4]) if len(sys.argv) > 4 else 3
        shown, inside = 0, False
        for depth, a, rest in rows:
            if a == want and not inside:
                if shown == limit:
                    break
                shown, inside, base = shown + 1, True, depth
                print(f"{a} {rest}")
            elif inside:
                if depth <= base:
                    inside = False
                    if a == want and shown < limit:
                        shown, inside, base = shown + 1, True, depth
                        print(f"{a} {rest}")
                else:
                    print("  " * (depth - base) + f"{a} {name.get(a, '')[:30]} {rest[:80]}")


if __name__ == "__main__":
    main()
