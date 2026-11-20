"""List this game's functions in an address range, with what d3d_match.py made of each.

    python d3d_list.py <lo hex> <hi hex> [min instructions]

Reads runs/d3d_match.csv and the function cache d3d_match.py left in runs/.
"""
import csv
import glob
import os
import pickle
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    least = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    funcs = None
    for path in glob.glob(os.path.join(ROOT, "runs", "d3d_match_*.pkl")):
        with open(path, "rb") as f:
            data = pickle.load(f)
        if len(data) > 70000:  # this game's (the known game's cache is smaller)
            funcs = data
    matched = {}
    with open(os.path.join(ROOT, "runs", "d3d_match.csv"), newline="") as f:
        for row in csv.DictReader(f):
            if row["here"]:
                matched[int(row["here"], 16)] = row
    for a in sorted(funcs):
        if not (lo <= a < hi) or len(funcs[a]) < least:
            continue
        row = matched.get(a)
        ops = funcs[a]
        calls = sum(1 for x in ops if x.startswith("bl "))
        note = f"<- {row['known']} {row['match']} {row['name']}" if row else ""
        print(f"{a:08X} {len(ops):5d} insns {calls:3d} calls  {note}")


if __name__ == "__main__":
    main()
