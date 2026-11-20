"""Reads the history an F4 capture wrote (capture_<stamp>\\history\\, src\\history.cpp): the five minutes before.

    python history.py <capture> summary                 how much there is, the frame times, the slowest frames
    python history.py <capture> stutters [ms=50] [n=20] the frames that took longer, each with what happened in it:
                                                        events (memory, files, slow calls, pipelines) and the
                                                        kernel calls of over 1 ms
    python history.py <capture> window <from> <to>      everything between two times (seconds before the capture,
                                                        "-12.5 -12.3"): frames, events, kernel calls, in order
    python history.py <capture> kernel [n=30]           the kernel functions by total time
    python history.py <capture> frame <number>          the graphics calls of one frame (render_calls.txt)

Times are seconds before the capture, the same in every file.
"""
import csv
import os
import re
import sys


def folder(path):
    inner = os.path.join(path, "history")
    return inner if os.path.isdir(inner) else path


def frames(h):
    with open(os.path.join(h, "frames.csv"), newline="") as f:
        return [dict(r, seconds=float(r["seconds"]), ms=float(r["ms"])) for r in csv.DictReader(f)]


def timed_lines(h, name):
    """(seconds, line) of a file whose lines start with the time."""
    out = []
    path = os.path.join(h, name)
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*(-?\d+\.\d+)\s", line)
            if m:
                out.append((float(m.group(1)), line.rstrip("\n")))
    return out


def kernel_ms(line):
    m = re.match(r"\s*-?\d+\.\d+\s+t\d+\s+(\d+\.\d+)", line)
    return float(m.group(1)) if m else 0.0


def between(lines, start, end):
    return [(t, l) for t, l in lines if start <= t <= end]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    h = folder(sys.argv[1])
    verb, args = sys.argv[2], sys.argv[3:]
    if verb == "summary":
        fr = frames(h)
        if fr:
            ms = sorted(f["ms"] for f in fr if f["ms"] > 0)
            span = -fr[0]["seconds"]
            print(f"{len(fr)} frames over {span:.0f} s: average {len(fr) / max(span, 0.001):.0f} fps, "
                  f"median {ms[len(ms) // 2]:.1f} ms, 99% {ms[len(ms) * 99 // 100]:.1f} ms, longest {ms[-1]:.0f} ms")
            for limit in (33, 50, 100, 250):
                print(f"  over {limit} ms: {sum(1 for m in ms if m > limit)}")
        for name in ("events.txt", "kernel_calls.txt", "render_calls.txt"):
            lines = timed_lines(h, name)
            if lines:
                print(f"{name}: {len(lines)} lines, from {lines[0][0]:.1f} s")
    elif verb == "stutters":
        limit = float(args[0]) if args else 50.0
        count = int(args[1]) if len(args) > 1 else 20
        fr = frames(h)
        events = timed_lines(h, "events.txt")
        kernel = [(t, l) for t, l in timed_lines(h, "kernel_calls.txt") if kernel_ms(l) >= 1.0]
        slow = sorted((f for f in fr if f["ms"] > limit), key=lambda f: -f["ms"])[:count]
        for f in sorted(slow, key=lambda f: f["seconds"]):
            end, start = f["seconds"], f["seconds"] - f["ms"] / 1000.0
            print(f"=== frame {f['frame']} at {end:.3f} s: {f['ms']:.0f} ms (renderer's thread "
                  f"{f['renderer_thread_ms']} ms, the game waited {f['game_waited_ms']} ms for it, built "
                  f"{f['built_ms']} ms, slowest call {f['slowest_call']} {f['slowest_ms']} ms; {f['draws']} draws)")
            for t, l in between(events, start, end):
                print("   event  " + l)
            for t, l in between(kernel, start - 0.001, end):
                print("   kernel " + l)
    elif verb == "window":
        start, end = float(args[0]), float(args[1])
        rows = [(f["seconds"], f"frame {f['frame']}: {f['ms']:.1f} ms, renderer {f['renderer_thread_ms']} ms, "
                               f"{f['draws']} draws") for f in frames(h) if start <= f["seconds"] <= end]
        rows += [(t, "event  " + l) for t, l in between(timed_lines(h, "events.txt"), start, end)]
        rows += [(t, "kernel " + l) for t, l in between(timed_lines(h, "kernel_calls.txt"), start, end)]
        for t, l in sorted(rows, key=lambda r: r[0]):
            print(l if l.startswith(("event", "kernel")) else f"{t:10.4f}  {l}")
    elif verb == "kernel":
        count = int(args[0]) if args else 30
        with open(os.path.join(h, "kernel_summary.txt")) as f:
            for i, line in enumerate(f):
                if i > count:
                    break
                sys.stdout.write(line)
    elif verb == "frame":
        want, on = f"--- frame {int(args[0])}", False
        with open(os.path.join(h, "render_calls.txt")) as f:
            for line in f:
                if line.startswith("--- frame"):
                    on = line.strip() == want
                if on:
                    sys.stdout.write(line)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
