"""A burst of renderer screenshots from a running test (route_run.ps1 -Native: XMD_TEST_SHOT_REQUEST), to catch
flicker: one picture cannot show it. Prints each frame's mean brightness and how far it is from the frame before
(mean absolute difference of a 64 x 36 thumbnail, 0-255); frames that jump are saved as PNG.

    python burst.py [count=20] [name=burst] [jump=12]     -> runs/<name>_NN.png for the frames that jump
"""
import os
import struct
import sys
import time
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RUNS = os.path.join(ROOT, "runs")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        start = pos
        while data[pos] > 32:
            pos += 1
        parts.append(data[start:pos])
        pos += 1
    return int(parts[1]), int(parts[2]), data[pos:]


def thumb(w, h, rgb, tw=64, th=36):
    out = bytearray()
    for y in range(th):
        row = (y * h // th) * w * 3
        for x in range(tw):
            at = row + (x * w // tw) * 3
            out += rgb[at:at + 3]
    return bytes(out)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))


def half(w, h, rgb):
    """Every second pixel of every second line (a quick small copy for looking at)."""
    out = bytearray()
    for y in range(0, h - 1, 2):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        out += b"".join(row[x * 3:x * 3 + 3] for x in range(0, w - 1, 2))
    return w // 2, h // 2, bytes(out)


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    name = sys.argv[2] if len(sys.argv) > 2 else "burst"
    jump = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
    request = os.path.join(RUNS, "shot_request.txt")
    last = None
    saved = 0
    for i in range(count):
        ppm = os.path.join(RUNS, f"{name}_{i:02d}.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        with open(request, "w") as f:
            f.write(ppm)
        for _ in range(100):
            if os.path.exists(ppm) and not os.path.exists(request):
                break
            time.sleep(0.03)
        if not os.path.exists(ppm):
            print(f"{i:02d}: no picture")
            continue
        w = h = 0
        for _ in range(60):  # (the game is still writing it: whole when its size is the header's)
            time.sleep(0.03)
            try:
                w, h, rgb = read_ppm(ppm)
                if len(rgb) >= w * h * 3:
                    os.remove(ppm)
                    break
            except (IndexError, ValueError, OSError):
                pass
            w = 0
        if not w:
            print(f"{i:02d}: unreadable")
            continue
        t = thumb(w, h, rgb)
        mean = sum(t) / len(t)
        diff = sum(abs(a - b) for a, b in zip(t, last)) / len(t) if last else 0.0
        mark = ""
        try:  # (XMD_NATIVE_DEBUG_CLEAR=1: pixels left at the clear colour)
            import numpy
            a = numpy.frombuffer(rgb, dtype=numpy.uint8, count=w * h * 3).reshape(h, w, 3).astype(numpy.int16)
            holes = int(((a[:, :, 0] > 140) & (a[:, :, 2] > 140) & (a[:, :, 1] < a[:, :, 0] // 3)).sum())
            mark = f"  magenta {holes}"
            if holes > 40 and saved < 8 and diff < jump and i:
                diff = jump
        except ImportError:
            pass
        if (diff >= jump or i == 0) and saved < int(os.environ.get("BURST_SAVE", "8")):
            sw, sh, small = half(w, h, rgb) if w > 1600 else (w, h, rgb)
            write_png(os.path.join(RUNS, f"{name}_{i:02d}.png"), sw, sh, small)
            saved += 1
            mark += "  <- saved"
        print(f"{i:02d}: {w}x{h} mean {mean:6.1f} diff {diff:5.1f}{mark}")
        last = t


if __name__ == "__main__":
    main()
