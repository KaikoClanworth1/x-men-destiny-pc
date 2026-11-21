"""Draws the button-prompt pictures the port shows in place of the game's Xbox 360 ones when the player is on
the keyboard and mouse, a PlayStation pad or a Nintendo pad (controls chat, port/src/pad_icons.cpp).

    python make_glyphs.py [out = ../res/glyphs.bin] [--sheet sheet.png]

Every picture is drawn here from plain shapes and letters (font: Roboto Bold, Apache License 2.0, from the SDK's
thirdparty/imgui folder) - nothing is taken from another game or from a platform holder's artwork.
128x128 RGBA each, like the game's own prompt images (Button1_img.dds ... Button15_img.dds).

File: "XMDG", u32 count, then count x { char name[24]; }, then count x 65536 bytes RGBA (little-endian).
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "..", "recomp", "rexglue-sdk", "thirdparty", "imgui", "misc", "fonts", "Roboto-Medium.ttf")
BOLD = os.path.join(HERE, "..", "..", "recomp", "rexglue-sdk", "thirdparty", "tracy", "profiler", "src", "font", "Roboto-Bold.ttf")
if os.path.exists(BOLD):
    FONT = BOLD
S = 4          # drawn at 4x, then scaled down
N = 128 * S


def canvas():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def shadow(shape_fn):
    """A soft dark halo behind a shape, as the game's own pictures have."""
    m = Image.new("L", (N, N), 0)
    shape_fn(ImageDraw.Draw(m), 255)
    m = m.filter(ImageFilter.GaussianBlur(5 * S))
    out = canvas()
    out.putalpha(m.point(lambda v: int(v * 0.55)))
    return out


def vgradient(top, bottom):
    g = Image.new("RGBA", (1, N))
    for y in range(N):
        t = y / (N - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g.resize((N, N))


def fill(mask_fn, top, bottom):
    m = Image.new("L", (N, N), 0)
    mask_fn(ImageDraw.Draw(m), 255)
    out = canvas()
    out.paste(vgradient(top, bottom), (0, 0), m)
    return out


def fit_font(text, max_w, max_h):
    size = int(max_h * 1.3)
    while size > 8:
        f = ImageFont.truetype(FONT, size)
        l, t, r, b = f.getbbox(text)
        if r - l <= max_w and b - t <= max_h:
            return f
        size -= 2
    return ImageFont.truetype(FONT, 8)


def put_text(img, text, box, colour):
    x0, y0, x1, y1 = box
    f = fit_font(text, x1 - x0, y1 - y0)
    l, t, r, b = f.getbbox(text)
    ImageDraw.Draw(img).text(((x0 + x1) / 2 - (l + r) / 2, (y0 + y1) / 2 - (t + b) / 2), text, font=f, fill=colour)


def finish(img):
    return img.resize((128, 128), Image.LANCZOS)


# --- shapes -----------------------------------------------------------------------------------------------

def keycap(text, wide=False, symbol=None):
    """A key: a light rounded cap with a dark legend."""
    w = 112 if wide else 88
    x0, x1 = (128 - w) / 2 * S, (128 + w) / 2 * S
    y0, y1 = 22 * S, 104 * S
    r = 14 * S
    body = lambda d, c: d.rounded_rectangle((x0, y0, x1, y1), r, fill=c)
    top = lambda d, c: d.rounded_rectangle((x0 + 5 * S, y0 + 4 * S, x1 - 5 * S, y1 - 10 * S), r - 5 * S, fill=c)
    img = shadow(body)
    img.alpha_composite(fill(body, (150, 150, 150), (88, 88, 88)))
    img.alpha_composite(fill(top, (238, 238, 238), (190, 190, 190)))
    # (a word fills the cap's width: the prompts are drawn 30 pixels high)
    inset = 7 if wide else 12
    box = (x0 + inset * S, y0 + 12 * S, x1 - inset * S, y1 - 18 * S)
    if symbol:
        symbol(ImageDraw.Draw(img), box, (30, 30, 30, 255))
    else:
        put_text(img, text, box, (30, 30, 30, 255))
    return finish(img)


def arrow(direction):
    def draw(d, box, colour):
        x0, y0, x1, y1 = box
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        a = min(x1 - x0, y1 - y0) * 0.42
        pts = [(0, -a), (a, a * 0.75), (-a, a * 0.75)]
        turn = {"up": 0, "right": 90, "down": 180, "left": 270}[direction]
        c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
        d.polygon([(cx + x * c - y * s, cy + x * s + y * c) for x, y in pts], fill=colour)
    return draw


def disc(text=None, symbol=None, rim=(215, 215, 215), face=((84, 84, 84), (36, 36, 36)), ink=(240, 240, 240, 255)):
    """A round button: light rim, dark face (the look of the game's own stick pictures)."""
    c = 64 * S
    outer = lambda d, col: d.ellipse((c - 50 * S, c - 50 * S, c + 50 * S, c + 50 * S), fill=col)
    inner = lambda d, col: d.ellipse((c - 41 * S, c - 41 * S, c + 41 * S, c + 41 * S), fill=col)
    img = shadow(outer)
    img.alpha_composite(fill(outer, rim, tuple(int(v * 0.55) for v in rim)))
    img.alpha_composite(fill(inner, face[0], face[1]))
    box = (c - 27 * S, c - 27 * S, c + 27 * S, c + 27 * S)
    if symbol:
        symbol(ImageDraw.Draw(img), box, ink)
    else:
        put_text(img, text, box, ink)
    return finish(img)


def pill(text=None, symbol=None):
    """A shoulder button or trigger: the keycap's shape, lower and wider."""
    x0, x1, y0, y1 = 14 * S, 114 * S, 34 * S, 94 * S
    body = lambda d, c: d.rounded_rectangle((x0, y0, x1, y1), 22 * S, fill=c)
    img = shadow(body)
    img.alpha_composite(fill(body, (232, 232, 232), (150, 150, 150)))
    box = (x0 + 14 * S, y0 + 10 * S, x1 - 14 * S, y1 - 12 * S)
    if symbol:
        symbol(ImageDraw.Draw(img), box, (30, 30, 30, 255))
    else:
        put_text(img, text, box, (30, 30, 30, 255))
    return finish(img)


def sym_cross(d, box, colour):
    x0, y0, x1, y1 = box
    d.line((x0, y0, x1, y1), fill=colour, width=7 * S)
    d.line((x0, y1, x1, y0), fill=colour, width=7 * S)


def sym_circle(d, box, colour):
    d.ellipse(box, outline=colour, width=7 * S)


def sym_square(d, box, colour):
    x0, y0, x1, y1 = box
    d.rectangle((x0 + 2 * S, y0 + 2 * S, x1 - 2 * S, y1 - 2 * S), outline=colour, width=7 * S)


def sym_triangle(d, box, colour):
    x0, y0, x1, y1 = box
    pts = [((x0 + x1) / 2, y0 - 2 * S), (x1 + 2 * S, y1 - 4 * S), (x0 - 2 * S, y1 - 4 * S)]
    d.line(pts + [pts[0], pts[1]], fill=colour, width=7 * S, joint="curve")


def sym_lines(d, box, colour):  # (a menu button: three lines)
    x0, y0, x1, y1 = box
    for k in range(3):
        y = y0 + (y1 - y0) * (0.2 + 0.3 * k)
        d.line((x0 + 8 * S, y, x1 - 8 * S, y), fill=colour, width=5 * S)


def sym_plus(d, box, colour):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    a = (y1 - y0) * 0.42
    d.line((cx - a, cy, cx + a, cy), fill=colour, width=7 * S)
    d.line((cx, cy - a, cx, cy + a), fill=colour, width=7 * S)


def sym_minus(d, box, colour):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    a = (y1 - y0) * 0.42
    d.line((cx - a, cy, cx + a, cy), fill=colour, width=7 * S)


def mouse(button=None, move=False):
    """A mouse seen from above; `button`: left / right / middle / side4 / side5 lit."""
    x0, x1, y0, y1 = 34 * S, 94 * S, 14 * S, 114 * S
    body = lambda d, c: d.rounded_rectangle((x0, y0, x1, y1), 30 * S, fill=c)
    img = shadow(body)
    img.alpha_composite(fill(body, (236, 236, 236), (160, 160, 160)))
    d = ImageDraw.Draw(img)
    lit, dark = (226, 60, 40, 255), (60, 60, 60, 255)
    split = y0 + 40 * S
    cx = (x0 + x1) / 2
    if button in ("left", "right"):
        m = Image.new("L", (N, N), 0)
        md = ImageDraw.Draw(m)
        md.rounded_rectangle((x0, y0, x1, y1), 30 * S, fill=255)
        md.rectangle((0, split, N, N), fill=0)
        md.rectangle((cx, 0, N, N) if button == "left" else (0, 0, cx, N), fill=0)
        img.paste(lit, (0, 0), m)
    d.line((x0 + 2 * S, split, x1 - 2 * S, split), fill=dark, width=3 * S)
    d.line((cx, y0 + 2 * S, cx, split), fill=dark, width=3 * S)
    d.rounded_rectangle((cx - 6 * S, y0 + 12 * S, cx + 6 * S, y0 + 32 * S), 6 * S,
                        fill=lit if button == "middle" else (120, 120, 120, 255), outline=dark, width=2 * S)
    if button in ("side4", "side5"):
        y = y0 + (62 if button == "side4" else 46) * S
        d.rounded_rectangle((x0 - 7 * S, y, x0 + 5 * S, y + 14 * S), 4 * S, fill=lit, outline=dark, width=2 * S)
    if move:
        for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
            ax, ay = cx + dx * 46 * S, 64 * S + dy * 58 * S
            a = 8 * S
            if dx:
                d.polygon([(ax + dx * a, ay), (ax - dx * a * 0.2, ay - a), (ax - dx * a * 0.2, ay + a)], fill=(240, 240, 240, 255))
            else:
                d.polygon([(ax, ay + dy * a * 0.9), (ax - a, ay - dy * a * 0.3), (ax + a, ay - dy * a * 0.3)], fill=(240, 240, 240, 255))
    return finish(img)


def cluster(labels):
    """Four small keys in the inverted-T of a keyboard's movement keys; labels: up, left, down, right."""
    img = canvas()
    cells = [(44, 14), (4, 56), (44, 56), (84, 56)]
    for (x, y), label in zip(cells, labels):
        x0, y0, x1, y1 = x * S, y * S, (x + 40) * S, (y + 40) * S
        body = lambda d, c, b=(x0, y0, x1, y1): d.rounded_rectangle(b, 8 * S, fill=c)
        top = lambda d, c, b=(x0 + 3 * S, y0 + 2 * S, x1 - 3 * S, y1 - 6 * S): d.rounded_rectangle(b, 6 * S, fill=c)
        img.alpha_composite(shadow(body))
        img.alpha_composite(fill(body, (150, 150, 150), (88, 88, 88)))
        img.alpha_composite(fill(top, (238, 238, 238), (190, 190, 190)))
        box = (x0 + 8 * S, y0 + 6 * S, x1 - 8 * S, y1 - 11 * S)
        if callable(label):
            label(ImageDraw.Draw(img), box, (30, 30, 30, 255))
        else:
            put_text(img, label, box, (30, 30, 30, 255))
    return finish(img)


# --- the set ----------------------------------------------------------------------------------------------

def build():
    g = {}
    # Keyboard: one picture a key, named as the port's key settings name it (controls_bind.cpp, KeyName).
    for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789":
        g["key_" + c] = keycap(c)
    words = {"Space": "SPACE", "Shift": "SHIFT", "Control": "CTRL", "Alt": "ALT", "Tab": "TAB", "Return": "ENTER",
             "Backspace": "BKSP", "Escape": "ESC", "Delete": "DEL", "Insert": "INS", "Home": "HOME", "End": "END",
             "PageUp": "PG UP", "PageDown": "PG DN"}
    for name, text in words.items():
        g["key_" + name] = keycap(text, wide=True)
    for name, text in {"Minus": "-", "Plus": "+", "Comma": ",", "Period": ".", "Semicolon": ";", "Slash": "/",
                       "Backslash": "\\", "LBracket": "[", "RBracket": "]", "Quote": "'"}.items():
        g["key_" + name] = keycap(text)
    for name, way in {"Up": "up", "Down": "down", "Left": "left", "Right": "right"}.items():
        g["key_" + name] = keycap("", symbol=arrow(way))
    for i in range(10):
        g["key_Numpad%d" % i] = keycap("NUM %d" % i, wide=True)
    for name, text in {"NumpadPlus": "NUM +", "NumpadMinus": "NUM -", "NumpadStar": "NUM *", "NumpadSlash": "NUM /"}.items():
        g["key_" + name] = keycap(text, wide=True)
    g["key_LMB"] = mouse("left")
    g["key_RMB"] = mouse("right")
    g["key_MMB"] = mouse("middle")
    g["key_Mouse4"] = mouse("side4")
    g["key_Mouse5"] = mouse("side5")
    g["key_unknown"] = keycap("?")
    g["kbm_camera"] = mouse(move=True)
    g["kbm_move_wasd"] = cluster(["W", "A", "S", "D"])
    g["kbm_move"] = cluster([arrow("up"), arrow("left"), arrow("down"), arrow("right")])
    # PlayStation pads (names: the Xbox control the picture stands in for). Plain white shapes.
    grey = ((84, 84, 84), (36, 36, 36))
    g["ps_a"] = disc(symbol=sym_cross, face=grey)
    g["ps_b"] = disc(symbol=sym_circle, face=grey)
    g["ps_x"] = disc(symbol=sym_square, face=grey)
    g["ps_y"] = disc(symbol=sym_triangle, face=grey)
    for name, text in {"lb": "L1", "rb": "R1", "lt": "L2", "rt": "R2"}.items():
        g["ps_" + name] = pill(text)
    g["ps_ls"] = disc("L3")
    g["ps_rs"] = disc("R3")
    g["ps_start"] = pill(symbol=sym_lines)
    g["ps_back"] = pill("SHARE")
    # Nintendo pads: the letters where that pad has them (A right, B bottom, X top, Y left).
    for name, text in {"a": "B", "b": "A", "x": "Y", "y": "X"}.items():
        g["nin_" + name] = disc(text)
    for name, text in {"lb": "L", "rb": "R", "lt": "ZL", "rt": "ZR"}.items():
        g["nin_" + name] = pill(text)
    g["nin_ls"] = disc("LS")
    g["nin_rs"] = disc("RS")
    g["nin_start"] = disc(symbol=sym_plus)
    g["nin_back"] = disc(symbol=sym_minus)
    # The sticks themselves (prompts like "move with the left stick") on PlayStation and Nintendo pads.
    g["pad_lstick"] = disc("L")
    g["pad_rstick"] = disc("R")
    # An Xbox-style pad whose buttons the player has swapped: the other button, in this set's own look.
    for name, text in {"a": "A", "b": "B", "x": "X", "y": "Y", "ls": "LS", "rs": "RS"}.items():
        g["xb_" + name] = disc(text)
    for name, text in {"lb": "LB", "rb": "RB", "lt": "LT", "rt": "RT", "start": "START", "back": "BACK"}.items():
        g["xb_" + name] = pill(text)
    return g


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else os.path.join(HERE, "..", "res", "glyphs.bin")
    g = build()
    names = sorted(g)
    with open(out, "wb") as f:
        f.write(b"XMDG" + struct.pack("<I", len(names)))
        for n in names:
            assert len(n) < 24, n
            f.write(n.encode().ljust(24, b"\0"))
        for n in names:
            f.write(g[n].tobytes())
    print(len(names), "pictures ->", os.path.abspath(out), os.path.getsize(out), "bytes")
    if "--sheet" in sys.argv:
        path = sys.argv[sys.argv.index("--sheet") + 1]
        cols = 14
        sheet = Image.new("RGBA", (cols * 132, ((len(names) + cols - 1) // cols) * 132), (60, 20, 20, 255))
        for i, n in enumerate(names):
            sheet.alpha_composite(g[n], ((i % cols) * 132 + 2, (i // cols) * 132 + 2))
        sheet.save(path)


if __name__ == "__main__":
    main()
