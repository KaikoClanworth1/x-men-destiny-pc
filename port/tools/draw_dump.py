"""A frame dump of the native renderer (runs/native_draws.txt, written when
runs/dump_now.txt appears in a test started by route_run.ps1 -Native), made
readable.

    python draw_dump.py                 runs of draws into one target, with their shaders
    python draw_dump.py draw <n> [c]    draw n in full (c: with its constants)
    python draw_dump.py ps <hash part>  the draws using a pixel shader
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FILE = os.path.join(ROOT, "runs", "native_draws.txt")


def draws():
    cur = None
    with open(FILE, encoding="latin-1") as f:
        for line in f:
            if line.startswith("=== resolve"):
                if cur:
                    yield cur
                    cur = None
                yield [line.rstrip("\n")]
            elif line.startswith("=== draw"):
                if cur:
                    yield cur
                cur = [line.rstrip("\n")]
            elif cur is not None:
                cur.append(line.rstrip("\n"))
    if cur:
        yield cur


def head(d):
    m = re.match(r"=== draw (\d+): prim (\d+) base (-?\d+) start (\d+) count (\d+) indexed (\d) \| vs (\w+) v(\d) ps (\w+)", d[0])
    t = next((l for l in d if l.startswith("target")), "")
    tm = re.match(r"target (\S+) fmt (\d+) \| vte (\w+) blend (\w+) colorctl (\w+) mask (\w+) \| surface (\w+) color (\w+) depth (\w+)", t)
    v = next((l for l in d if l.startswith("viewport")), "")
    dm = re.search(r"depthctl (\w+)", v)
    return dict(n=int(m.group(1)), prim=int(m.group(2)), count=int(m.group(5)), vs=m.group(7), ps=m.group(9),
                target=tm.group(1) if tm else "?", color=tm.group(8) if tm else "?", blend=tm.group(4) if tm else "?",
                depthctl=dm.group(1) if dm else "?", mask=tm.group(6) if tm else "?")


def main():
    # (a first argument that is a file: that dump - an F4 capture's draws.txt - instead of runs/native_draws.txt)
    global FILE
    if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]):
        FILE = sys.argv.pop(1)
    mode = sys.argv[1] if len(sys.argv) > 1 else "runs"
    if mode == "draw":
        want = int(sys.argv[2])
        consts = len(sys.argv) > 3
        for d in draws():
            if d[0].startswith("=== draw") and head(d)["n"] == want:
                for l in d:
                    if consts or not re.match(r"  [gp]c\d+ = ", l):
                        print(l[:240])
                return
        return
    if mode == "ps":
        for d in draws():
            if not d[0].startswith("=== draw"):
                continue
            h = head(d)
            if sys.argv[2].upper() in h["ps"]:
                print(h)
        return
    run = None
    for d in draws():
        if d[0].startswith("=== resolve"):
            if run:
                show(run)
                run = None
            print(d[0][4:200])
            continue
        h = head(d)
        key = (h["target"], h["color"])
        if run is None or run["key"] != key:
            if run:
                show(run)
            run = dict(key=key, first=h["n"], last=h["n"], shaders=collections.Counter(), prims=collections.Counter())
        run["last"] = h["n"]
        run["shaders"][(h["vs"][:8], h["ps"][:8], h["blend"], h["depthctl"])] += 1
        run["prims"][h["prim"]] += 1
    if run:
        show(run)


def show(run):
    n = run["last"] - run["first"] + 1
    print("draws %d-%d (%d) into %s color %s prims %s" % (run["first"], run["last"], n, run["key"][0], run["key"][1],
                                                         dict(run["prims"])))
    if n <= 6 or len(run["shaders"]) <= 6:
        for (vs, ps, blend, depth), c in run["shaders"].items():
            print("     %4d x vs %s ps %s blend %s depth %s" % (c, vs, ps, blend, depth))


if __name__ == "__main__":
    main()
