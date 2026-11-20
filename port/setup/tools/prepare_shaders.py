"""Prepare the native renderer's shaders on the player's PC, from their own copy of the game.

    python prepare_shaders.py <game folder> <work folder> <native_shaders folder> [--jobs=N] [--targets=dxil,spirv,dxbc]
                              [--extra=<folder of .xsc files>]

Runs the port's own tools unchanged (port/tools/find_shaders.py, convert_shaders.py): this file sits in the folder
they take as the project root, which holds what they look for - recomp/XenosRecomp (the converter, its header, DXC)
and port/src/native/shaders (the renderer's own shaders). The XMD Setup package is laid out that way
(sdk/shadertools), and so is the developer tree when this file is copied to its root.

1. finds the game's shader containers in its data files and inside Default.xex -> <work>/shaders_disc/*.xsc
2. converts them to HLSL, then DXIL (Direct3D 12), SPIR-V (Vulkan) and DXBC (Direct3D 11) -> <work>/shaders_native
3. packs each kind into <native_shaders>/shaders<extension>.pak, the files the game loads

The game also compiles some shaders of its own while it runs (its program holds a shader compiler and their source):
those exist nowhere in its files. The running game saves each one it has no converted shader for; --extra names the
folder they are in, and they are converted and packed with the rest (then removed from that folder).

Every step carries on where an earlier run stopped. Progress lines on stdout: "progress <step> <done> <total>".
Exit code 0: the packs are written (shaders that failed to convert are listed in <work>/shaders_native/report.txt).
"""
import ctypes
import glob
import os
import struct
import sys
import threading

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "port", "tools"))

# Left alone in the game folder: what the port and the game write there.
SKIP_DIRS = {"userdata", "native_shaders", "logs", "cmdbuf", "crashes", "captures", "cache"}
SKIP_EXTENSIONS = (".exe", ".dll", ".pdb", ".usm", ".xma", ".log", ".toml", ".ini", ".lnk")
# The files of one kind of shader that go into its pack (besides the shaders): what the renderer reads about them.
SIDE_FILES = (".textures", ".inputs", ".consts", ".indexed")
PACKS = {"dxil": (".dxil",), "spirv": (".spv", ".spvc"), "dxbc": (".dxbc", ".dxbc4")}


def say(text):
    print(text, flush=True)


# ---- Default.xex: the game's program holds a few shaders itself. Its image is AES-128-CBC encrypted (the title's
# key, itself encrypted with the console family's retail key) and stored as blocks of data with runs of zeros
# left out ("basic" compression).
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")


def aes_cbc_decrypt(key, data):
    """AES-128-CBC with a zero IV, by Windows (bcrypt.dll)."""
    bcrypt = ctypes.WinDLL("bcrypt")
    alg, handle = ctypes.c_void_p(), ctypes.c_void_p()
    if bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(alg), ctypes.c_wchar_p("AES"), None, 0):
        raise OSError("AES is not available")
    mode = ctypes.create_unicode_buffer("ChainingModeCBC")
    bcrypt.BCryptSetProperty(alg, ctypes.c_wchar_p("ChainingMode"), mode, ctypes.sizeof(mode), 0)
    if bcrypt.BCryptGenerateSymmetricKey(alg, ctypes.byref(handle), None, 0, key, len(key), 0):
        raise OSError("AES key refused")
    iv = ctypes.create_string_buffer(16)
    out = ctypes.create_string_buffer(len(data))
    done = ctypes.c_ulong(0)
    status = bcrypt.BCryptDecrypt(handle, data, len(data), None, iv, 16, out, len(data), ctypes.byref(done), 0)
    bcrypt.BCryptDestroyKey(handle)
    bcrypt.BCryptCloseAlgorithmProvider(alg, 0)
    if status:
        raise OSError("AES decrypt failed (0x%08X)" % (status & 0xFFFFFFFF))
    return out.raw[:done.value]


def xex_image(path):
    """The program's image as it is loaded, or None when the file is not packed the way this game's is."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"XEX2":
        return None
    pe_offset, security, count = struct.unpack_from(">I4xII", data, 8)
    fmt = None
    for i in range(count):
        key, value = struct.unpack_from(">II", data, 24 + 8 * i)
        if key == 0x3FF:
            fmt = value
    if fmt is None:
        return None
    size, encryption, compression = struct.unpack_from(">IHH", data, fmt)
    body = data[pe_offset:]
    if encryption == 1:
        title_key = aes_cbc_decrypt(RETAIL_KEY, data[security + 0x150:security + 0x160])
        body = aes_cbc_decrypt(title_key, body[:len(body) & ~15])
    if compression == 0:
        return body
    if compression != 1:
        return None  # (LZX: not this game's)
    image, at = bytearray(), 0
    for i in range((size - 8) // 8):
        length, zeros = struct.unpack_from(">II", data, fmt + 8 + 8 * i)
        image += body[at:at + length]
        image += bytes(zeros)
        at += length
    return bytes(image)


# ---- 1. the containers
def find(game, out):
    import gzip
    import find_shaders as fs

    os.makedirs(out, exist_ok=True)
    files = []
    for root, dirs, names in os.walk(game):
        if os.path.normcase(root) == os.path.normcase(game):
            dirs[:] = [d for d in dirs if d.lower() not in SKIP_DIRS]
        for name in sorted(names):
            if not name.lower().endswith(SKIP_EXTENSIONS):
                files.append(os.path.join(root, name))
    seen = {os.path.basename(f)[:16] for f in glob.glob(os.path.join(out, "*.xsc"))}
    found = 0

    def keep(data):
        nonlocal found
        for at, size, flags in fs.scan(data):
            blob = data[at:at + size]
            key = "%016X" % fs.fnv1a(blob)
            found += 1
            if key in seen:
                continue
            seen.add(key)
            part = os.path.join(out, key + ".part")
            with open(part, "wb") as f:
                f.write(blob)
            os.replace(part, os.path.join(out, "%s.%s.xsc" % (key, "vs" if flags & 1 else "ps")))

    for n, path in enumerate(files):
        say("progress find %d %d" % (n, len(files)))
        try:
            if path.lower().endswith(".xex"):
                data = xex_image(path)
                if data is None:
                    say("note %s is packed in a way this tool does not read: its shaders are left out" % os.path.basename(path))
                    continue
            elif path.lower().endswith(".gz"):
                with gzip.open(path, "rb") as f:
                    data = f.read()
            else:
                with open(path, "rb") as f:
                    data = f.read()
        except (OSError, EOFError) as e:
            say("note %s: %s" % (os.path.basename(path), e))
            continue
        keep(data)
    say("progress find %d %d" % (len(files), len(files)))
    say("note %d shader containers in the game's files, %d distinct" % (found, len(seen)))
    return len(seen)


# ---- 2. the conversion, by the port's own converter
def convert(xsc_dir, out, jobs, targets):
    os.environ["XMD_JOBS"] = str(jobs)
    os.environ["XMD_SHADER_TARGETS"] = ",".join(targets)
    import convert_shaders as cs

    # Direct3D 11's compiler comes with the Windows SDK of the C++ tools setup found (their environment is ours).
    sdk_bin = os.environ.get("WindowsSdkVerBinPath", "")
    fxc = os.path.join(sdk_bin, "x64", "fxc.exe")
    if sdk_bin and os.path.exists(fxc):
        cs.FXC = fxc
    # (the converter loads DXC's DLL: its folder on PATH, as it is not beside the converter in the package)
    os.environ["PATH"] = os.path.dirname(cs.DXC) + os.pathsep + os.environ.get("PATH", "")
    for need in (cs.XR, cs.HEADER, cs.DXC):
        if not os.path.exists(need):
            say("error the setup is incomplete: %s is missing" % os.path.relpath(need, ROOT))
            return False
    if "dxbc" in targets and not (cs.FXC and os.path.exists(cs.FXC)):
        say("note fxc.exe (Windows SDK) was not found: the Direct3D 11 shaders are left out")
    total = len(glob.glob(os.path.join(xsc_dir, "*.xsc")))
    done = [0]
    lock = threading.Lock()
    inner = cs.convert

    def counted(xsc, out_dir):
        result = inner(xsc, out_dir)
        with lock:
            done[0] += 1
            if done[0] % 8 == 0 or done[0] == total:
                say("progress convert %d %d" % (done[0], total))
        return result

    cs.convert = counted
    say("progress convert 0 %d" % total)
    argv = sys.argv
    sys.argv = [argv[0], out, xsc_dir]
    try:
        cs.main()
    finally:
        sys.argv = argv
        cs.convert = inner
    return True


# ---- 3. the packs: "SVRPAK1\0", u32 count, then per file u16 name length, name, u32 offset, u32 size; then the
# files (native_renderer.cpp Pack).
def pack(converted, out, targets):
    os.makedirs(out, exist_ok=True)
    written = 0
    for kind in targets:
        folder = os.path.join(converted, kind)
        for ext in PACKS[kind]:
            names = sorted(n for n in os.listdir(folder) if n.endswith(ext) or n.endswith(SIDE_FILES))
            if not any(n.endswith(ext) for n in names):
                continue
            table = 12 + sum(2 + len(n.encode()) + 8 for n in names)
            sizes = [os.path.getsize(os.path.join(folder, n)) for n in names]
            if table + sum(sizes) >= 1 << 32:
                say("error the %s shaders are too large for one pack" % ext)
                return 0
            part = os.path.join(out, "shaders%s.pak.part" % ext)
            with open(part, "wb") as f:
                f.write(b"SVRPAK1\0" + struct.pack("<I", len(names)))
                at = table
                for n, size in zip(names, sizes):
                    raw = n.encode()
                    f.write(struct.pack("<H", len(raw)) + raw + struct.pack("<II", at, size))
                    at += size
                for n in names:
                    with open(os.path.join(folder, n), "rb") as src:
                        f.write(src.read())
            os.replace(part, os.path.join(out, "shaders%s.pak" % ext))
            written += 1
            say("note shaders%s.pak: %d files, %.1f MB" % (ext, len(names), (table + sum(sizes)) / 2 ** 20))
    return written


def fs_scan(data):
    import find_shaders as fs
    return fs.scan(data)


def fs_fnv1a(data):
    import find_shaders as fs
    return fs.fnv1a(data)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 3:
        say(__doc__)
        return 2
    game, work, out = (os.path.abspath(a) for a in args)

    def option(name, default):
        for a in sys.argv:
            if a.startswith("--" + name + "="):
                return a.split("=", 1)[1]
        return default

    jobs = max(1, int(option("jobs", "8")))
    targets = [t for t in option("targets", "dxil,spirv,dxbc").split(",") if t in PACKS]
    xsc = os.path.join(work, "shaders_disc")
    converted = os.path.join(work, "shaders_native")
    stamp = os.path.join(xsc, "found.ok")  # (the game's files do not change: found once)
    if os.path.exists(stamp):
        say("note the shader containers were found by an earlier run")
    else:
        if not find(game, xsc):
            say("error no shaders were found in %s" % game)
            return 1
        open(stamp, "w").close()
    # the shaders the running game saved (see the top of this file)
    extra = option("extra", "")
    taken = []
    if extra and os.path.isdir(extra):
        for path in sorted(glob.glob(os.path.join(extra, "*.xsc"))):
            with open(path, "rb") as f:
                data = f.read()
            hits = fs_scan(data)
            if len(hits) != 1 or hits[0][0] != 0 or hits[0][1] != len(data):
                say("note %s is not a shader container: left alone" % os.path.basename(path))
                continue
            name = "%016X.%s.xsc" % (fs_fnv1a(data), "vs" if data[3] & 1 else "ps")
            with open(os.path.join(xsc, name), "wb") as f:
                f.write(data)
            taken.append(path)
        say("note %d shaders saved by the game are added" % len(taken))
    if not convert(xsc, converted, jobs, targets):
        return 1
    packs = pack(converted, out, targets)
    if not packs:
        say("error no shader could be converted (see %s)" % os.path.join(converted, "report.txt"))
        return 1
    for path in taken:  # (only now: a failed run leaves them for the next one)
        try:
            os.remove(path)
        except OSError:
            pass
    say("progress done 1 1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
