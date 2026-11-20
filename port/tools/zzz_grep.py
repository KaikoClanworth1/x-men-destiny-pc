"""Search the game's gzip-packed archives (*.zzz.gz, *.gNN.gz) for a byte string.

    python zzz_grep.py <game folder> <text> [name filter]

Prints each archive holding the text, the offset of the first hit and a little
of what surrounds it.
"""
import gzip
import os
import sys


def main():
    folder, needle = sys.argv[1], sys.argv[2].encode("latin-1")
    name_filter = sys.argv[3].lower() if len(sys.argv) > 3 else ""
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith(".gz") or name_filter not in name.lower():
            continue
        try:
            with gzip.open(os.path.join(folder, name), "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"{name}: {e}")
            continue
        at = data.find(needle)
        if at < 0:
            continue
        lo, hi = max(0, at - 48), min(len(data), at + len(needle) + 80)
        context = "".join(chr(c) if 32 <= c < 127 else "." for c in data[lo:hi])
        print(f"{name} ({len(data)} bytes) @0x{at:X} x{data.count(needle)}: {context}")


if __name__ == "__main__":
    main()
