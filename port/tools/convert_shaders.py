"""Convert the game's shader containers to HLSL (XenosRecomp), then DXIL and
SPIR-V (DXC) and DXBC (fxc) - the native renderer's shaders (src/native).

    python convert_shaders.py [out_dir] [xsc_dir ...]

Defaults: runs/shaders_disc (tools/find_shaders.py: the disc's) and
runs/shaders (the few the program itself holds, saved by a probe build with
XMD_SHADER_DUMP) -> runs/shaders_native/{hlsl,dxil,spirv,dxbc}.
Output names are the hashes the renderer computes from the game's shader
objects (native_renderer.cpp FindShader): a pixel shader's is the FNV-1a of
its whole container, a vertex shader's of its container's virtual part with
flag 0x40 cleared (binding a vertex shader to a vertex format rewrites its
microcode).
  dxil/<hash>.<vs|ps>.dxil        specialization constants = 0
  dxil/<hash>.vs.s1.dxil          packed 11:11:10 normals (if the shader has normals)
  dxil/<hash>.vs.s32.dxil         normals as signed bytes
  dxil/<hash>.vs.s64.dxil         packed 10:11:11 normals
  dxil/<hash>.ps.s2.dxil          alpha test (if the shader has it)
  dxil/<hash>.vs.inputs           the vertex shader's inputs: SEMANTIC INDEX TYPE LOCATION
  dxil/<hash>.<vs|ps>.textures    the texture fetch slots it samples: SLOT DIMENSION
spirv/ has the same as .spv / .spvc (Vulkan), dxbc/ as .dxbc / .dxbc4
(Direct3D 11). XMD_SHADER_TARGETS=dxil,spirv,dxbc chooses which are built
(default all); shaders whose files are there already are skipped.
XMD_JOBS: how many at once (default 8). Writes out_dir/report.txt.
The converter is the SvR 2011 port's patched XenosRecomp (..\\recomp\\XenosRecomp):
its SVR_* names in the HLSL are that patch's.
"""
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
XR = os.path.join(ROOT, "recomp", "XenosRecomp", "build", "XenosRecomp", "XenosRecomp.exe")
HEADER = os.path.join(ROOT, "recomp", "XenosRecomp", "XenosRecomp", "shader_common.h")
TARGETS = set(os.environ.get("XMD_SHADER_TARGETS", "dxil,spirv,dxbc").split(","))


def fnv1a(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


USAGE_VARIABLES = ["Position", "BlendWeight", "BlendIndices", "Normal", "PointSize", "TexCoord", "Tangent",
                   "Binormal", "TessFactor", "PositionT", "Color", "Fog", "Depth", "Sample"]
USAGE_SEMANTICS = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                   "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]
FETCH_USE_RE = re.compile(r"\bi(" + "|".join(USAGE_VARIABLES) + r")(\d+)\b")

# Indexed vertex fetches. Skinned models keep their bone matrices in a vertex
# stream, and the shader fetches rows of it by bone number (a fetch whose
# index is a register the shader computed, not the vertex number). Vertex
# inputs can't do that: those fetches read rows the renderer passes in a
# constant buffer instead (native_renderer.cpp IndexedRows): row 0 holds what
# index values are multiplied by, then row 1 + number * (elements fetched
# this way) + (which element).
INDEXED_HELPER = """
// [xmd] rows of a vertex stream fetched by a computed index (bone matrices)
#ifdef __spirv__
#define XMD_INDEXED_ROW(I) asfloat(g_ConstantBuffer.Load4(g_PushConstants.OwnConstants + (I) * 16))
#elif defined(SVR_D3D11)
cbuffer XmdIndexedRows : register(b3) { float4 g_XmdIndexedRows[4096]; };
#define XMD_INDEXED_ROW(I) g_XmdIndexedRows[I]
#else
cbuffer XmdIndexedRows : register(b3, space4) { float4 g_XmdIndexedRows[4096]; };
#define XMD_INDEXED_ROW(I) g_XmdIndexedRows[I]
#endif
#define XMD_INDEXED(K, VALUE) XMD_INDEXED_ROW(min(1u + uint(max(0.0, (VALUE) * XMD_INDEXED_ROW(0).x + 0.5)) * %d + (K), 4095u))
"""


def indexed_fetches(xsc):
    """Per vertex fetch in program order: (usage, usage index, source register expression or None)."""
    with open(xsc, "rb") as f:
        data = f.read()
    vsize = int.from_bytes(data[4:8], "big")
    shader_at = int.from_bytes(data[0x18:0x1C], "big")
    word = lambda at: int.from_bytes(data[at:at + 4], "big")
    code = vsize + word(shader_at)
    first, count = word(shader_at + 0x18), word(shader_at + 0x1C)
    fetches = []
    for i in range(count):
        v = word(shader_at + 0x24 + 4 * (first + i))
        fetches.append((v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF))
    fetches.sort()
    out, source = [], None
    for address, usage, index in fetches:
        w0, w1 = word(code + 12 * address), word(code + 12 * address + 4)
        if not (w1 >> 30) & 1:  # (a mini fetch reads on with the full fetch's index)
            source = ((w0 >> 5) & 0x3F, (w0 >> 30) & 3)
        out.append((usage, index, source))
    if not out:
        return out
    # The vertex number's register: the one the position is fetched with (else the first fetch's).
    vertex = next((src for u, i, src in out if u == 0 and i == 0), out[0][2])
    return [(u, i, None if src == vertex else "r%d.%s" % (src[0], "xyzw"[src[1]])) for u, i, src in out]


USAGE_NAMES = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
               "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]


def fetch_targets(xsc):
    """Per vertex element (SEMANTIC, index): the register its fetch writes and which components (bits 0-3).

    The game's graphics library reorders a shader's fetches when it binds it to a vertex format, so the renderer
    tells what a fetch is for by the register it writes (native_renderer.cpp Declaration)."""
    with open(xsc, "rb") as f:
        data = f.read()
    vsize = int.from_bytes(data[4:8], "big")
    shader_at = int.from_bytes(data[0x18:0x1C], "big")
    word = lambda at: int.from_bytes(data[at:at + 4], "big")
    code = vsize + word(shader_at)
    first, count = word(shader_at + 0x18), word(shader_at + 0x1C)
    out = {}
    for i in range(count):
        v = word(shader_at + 0x24 + 4 * (first + i))
        address, usage, index = v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF
        w0, w1 = word(code + 12 * address), word(code + 12 * address + 4)
        mask = sum(1 << k for k in range(4) if (w1 >> (3 * k)) & 7 != 7)
        if usage < len(USAGE_NAMES):
            out.setdefault((USAGE_NAMES[usage], index), ((w0 >> 12) & 0x3F, mask, w1 & 0xFFF))
    return out


def stamp_inputs(xsc, out):
    """<name>.inputs with each input's register and components added (SEMANTIC INDEX TYPE LOCATION REG MASK SWIZZLE)."""
    name = output_name(xsc)
    targets = fetch_targets(xsc)
    for d in ("dxil", "spirv", "dxbc"):
        path = os.path.join(out, d, name + ".inputs")
        if not os.path.exists(path):
            continue
        lines = []
        for line in open(path).read().splitlines():
            f = line.split()
            if len(f) >= 4:
                reg, mask, swizzle = targets.get((f[0], int(f[1])), (-1, 0, 0x688))
                line = " ".join(f[:4] + [str(reg), str(mask), str(swizzle)])
            if line:
                lines.append(line)
        with open(path, "w") as fo:
            fo.write("".join(l + chr(10) for l in lines))


def stamp_consts(xsc, out):
    """<name>.consts: how many float4 constants of its stage the shader declares (the highest register + 1).

    The renderer uploads that many for a draw instead of all 256 (native_renderer.cpp GpuConstants)."""
    name = output_name(xsc)
    path = os.path.join(out, "hlsl", name + ".hlsl")
    if not os.path.exists(path):
        return None
    text = open(path, encoding="utf-8", errors="replace").read()
    at = text.find("cbuffer %sShaderConstants" % ("Vertex" if ".vs" in name else "Pixel"))
    if at < 0:
        return None
    block = text[at:text.index("};", at)]
    count = 0
    for m in re.finditer(r"float4\s+\w+(?:\[(\d+)\])?\s*:\s*packoffset\(c(\d+)\)", block):
        count = max(count, int(m.group(2)) + int(m.group(1) or 1))
    # (anything else declared there: not understood - the renderer then uploads them all)
    declared = len(re.findall(r"packoffset\(", block))
    if declared != len(re.findall(r"float4\s+\w+(?:\[\d+\])?\s*:\s*packoffset\(c\d+\)", block)):
        return None
    for d in ("dxil", "spirv", "dxbc"):
        if os.path.isdir(os.path.join(out, d)):
            with open(os.path.join(out, d, name + ".consts"), "w") as f:
                f.write("%d\n" % count)
    return count


def rewrite_indexed(src, fetches):
    """The converted shader with its indexed fetches reading constant rows. Returns (source, [(semantic, index)])."""
    order = []  # the elements fetched by index, in the order of their rows
    for usage, index, source in fetches:
        if source and (usage, index) not in order:
            order.append((usage, index))
    if not order:
        return src, []
    start = src.find("void main(")
    open_at = src.find("{", start)
    head, body = src[:open_at], src[open_at:]
    lines = body.split("\n")
    n = 0
    for at, line in enumerate(lines):
        m = FETCH_USE_RE.search(line)
        if not m or "=" not in line:
            continue
        if n >= len(fetches):
            return None, "more fetch statements than fetch instructions"
        usage, index, source = fetches[n]
        n += 1
        if m.group(1) != USAGE_VARIABLES[usage] or int(m.group(2)) != index:
            return None, f"fetch {n}: statement reads {m.group(0)}, instruction {USAGE_VARIABLES[usage]}{index}"
        if source:
            lines[at] = line[:m.start()] + f"XMD_INDEXED({order.index((usage, index))}, {source})" + line[m.end():]
    if n != len(fetches):
        return None, f"{n} fetch statements, {len(fetches)} fetch instructions"
    # Their inputs go (unless a plain fetch reads the element too).
    plain = {(u, i) for u, i, source in fetches if not source}
    kept = []
    for line in head[start:].split("\n"):
        m = INPUT_RE.search(line)
        if m:
            key = (USAGE_SEMANTICS.index(m.group(3)), int(m.group(4)))
            if key in order and key not in plain:
                continue
        kept.append(line)
    # (before the function's [shader("vertex")] attribute - texture defines may follow it - when it has one)
    at = head.rfind("[shader(", 0, start)
    if at < 0:
        at = start
    elif head.rfind("#ifndef __spirv__", 0, at) >= at - 24:
        at = head.rfind("#ifndef __spirv__", 0, at)
    head = head[:at] + (INDEXED_HELPER % len(order)) + head[at:start] + "\n".join(kept)
    return head + "\n".join(lines), [(USAGE_SEMANTICS[u], i) for u, i in order]


def output_name(xsc):
    """<hash>.<vs|ps> as the renderer names the shader."""
    with open(xsc, "rb") as f:
        data = f.read()
    if data[3] & 1:  # vertex
        vsize = int.from_bytes(data[4:8], "big")
        virtual = data[:3] + bytes([data[3] & 0xBF]) + data[4:vsize]
        return f"{fnv1a(virtual):016X}.vs"
    return f"{fnv1a(data):016X}.ps"
DXC = os.path.join(ROOT, "recomp", "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
# fxc (Direct3D 11's DXBC): the newest Windows SDK's.
FXC = (sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe")) or [""])[-1]

INPUT_RE = re.compile(r"(?:\[\[vk::location\((\d+)\)\]\] )?in (float4|uint4) i\w+ : ([A-Z]+)(\d+)")
TEXTURE_DEFINE_RE = re.compile(r"#define (\w+)_Texture(2D|3D|Cube)DescriptorIndex g_ResourceIndex\(\d+, (\d+)\)")
TEXTURE_USE_RE = re.compile(r"(\w+)_Texture(2D|3D|Cube)DescriptorIndex")
DIMENSIONS = {"2D": 0, "3D": 1, "Cube": 2}


def compile_dxil(hlsl, dxil, stage, mask):
    r = subprocess.run([DXC, "-nologo", "-HV", "2021", "-T", f"{stage}_6_0", "-E", "main",
                        f"-DSVR_SPEC_CONSTANTS={mask}", "-Fo", dxil, hlsl],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


def compile_spirv(hlsl, spv, stage, mask, compact=False):
    flags = ["-fvk-invert-y"] if stage == "vs" else ["-fvk-use-dx-position-w"]
    if compact:
        flags.append("-DSVR_COMPACT_TABLES")
    r = subprocess.run([DXC, "-nologo", "-HV", "2021", "-T", f"{stage}_6_0", "-E", "main", "-spirv",
                        "-fvk-use-dx-layout", "-fspv-target-env=vulkan1.1", *flags,
                        f"-DSVR_SPEC_CONSTANTS={mask}", "-Fo", spv, hlsl],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


# fxc reads an older HLSL than DXC: no [[vk::...]] attributes, register
# spaces, [shader()] attributes or empty-parameter macros, and a macro NAME(i)
# can't share its name with the array NAME[] it reads.
ARRAY_MACRO_RE = re.compile(r"#define (\w+)\(INDEX\)")


def fxc_source(text):
    text = re.sub(r"\[\[vk::[^\]]*\]\]", "", text)
    text = text.replace(", space4)", ")")
    text = re.sub(r'\[shader\("\w+"\)\]', "", text)
    text = text.replace("#define DEFINE_SHARED_CONSTANTS() \\", "#define DEFINE_SHARED_CONSTANTS \\")
    text = text.replace("DEFINE_SHARED_CONSTANTS();", "DEFINE_SHARED_CONSTANTS")
    for name in set(ARRAY_MACRO_RE.findall(text)):
        text = re.sub(r"\b%s\[" % re.escape(name), name + "_a[", text)
    return finite_clamps(text)


# The converted shaders keep log2 / rcp / rsqrt results finite as the Xenos
# does: clamp(rsqrt(x), FLT_MIN, FLT_MAX) (FLT_MIN being -FLT_MAX there). fxc
# takes floats as finite and drops such a clamp, so rsqrt(0) stayed infinite and
# a zero vector normalised (0 * inf) was NaN - D3D11 drew the ringside floor and
# the barricade tops white. The clamps go through a function on the float's bits
# (integer operations: nothing fxc or a driver can fold away): the magnitude
# capped at FLT_MAX, the sign kept - +-inf (and NaN) become +-FLT_MAX, finite
# values pass bit for bit. (The first version, a `precise` float clamp, broke
# the wrestlers' skin on Fermi GPUs - GT 430, report 20261102-175019-77d3c2.)
FINITE_HELPER = ("float SvrFinite(float x) { uint u = asuint(x); "
                 "return asfloat((u & 0x80000000u) | min(u & 0x7fffffffu, 0x7f7fffffu)); }\n")
FINITE_TAIL = ", FLT_MIN, FLT_MAX)"


def finite_clamps(text):
    text = finite_calls(text)
    start = text.find("void main(")
    if "SvrFinite(" not in text or start < 0:
        return text
    return text[:start] + FINITE_HELPER + text[start:]


def finite_calls(text):
    out, pos = [], 0
    while True:
        start = text.find("clamp(", pos)
        if start < 0:
            break
        depth, k = 1, start + len("clamp(")
        while k < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[k], 0)
            k += 1
        call = text[start:k]
        if call.endswith(FINITE_TAIL):
            inner = finite_calls(call[len("clamp("):-len(FINITE_TAIL)])
            out.append(text[pos:start] + "SvrFinite(" + inner + ")")
            pos = k
        else:  # (another clamp: its arguments may hold some)
            out.append(text[pos:start + len("clamp(")])
            pos = start + len("clamp(")
    out.append(text[pos:])
    return "".join(out)


# Shader Model 4.0 (feature level 10_x GPUs) has 16 interpolator registers;
# the converted shaders declare 24 (TEXCOORD0-15, COLOR0-7) of which the game's
# pixel shaders read only these. Every shader keeps them in this order (so a
# vertex shader's outputs line up with any pixel shader's inputs) and the rest
# become plain variables.
SM4_INTERPOLATORS = [f"TEXCOORD{i}" for i in range(8)] + ["COLOR0", "COLOR3", "COLOR4"]
PARAM_RE = re.compile(r"^\s*(in|out) float4 (\w+) : ([A-Z]+\d+)$")


def sm4_source(text):
    start = text.find("void main(")
    if start < 0:
        return text, None
    end = text.find(")", start)
    params = text[start + len("void main("):end].split(",")
    kept, dropped = [], []
    for p in params:
        m = PARAM_RE.match(p.strip("\r\n"))
        if m and m.group(3) not in SM4_INTERPOLATORS and m.group(3).startswith(("TEXCOORD", "COLOR")):
            dropped.append(m)
        else:
            kept.append(p)
    body = text[end:]
    for m in dropped:
        if m.group(1) == "in" and re.search(r"\b%s\b" % m.group(2), body):
            return text, f"sm4: reads {m.group(3)}"
    statics = "".join(f"static float4 {m.group(2)} = 0;\n" for m in dropped)
    # (rcp() is Shader Model 5 only)
    return ("#define rcp(X) (1.0 / (X))\n" + text[:start] + statics + "void main(" + ",".join(kept) + body,
            None)


def compile_dxbc(hlsl, dxbc, stage, mask, fixed_dir, model="5_0"):
    fixed = os.path.join(fixed_dir, os.path.basename(hlsl))
    with open(hlsl, encoding="utf-8", errors="ignore") as f:
        text = fxc_source(f.read())
    if model == "4_0":
        fixed = fixed[:-5] + ".sm4.hlsl"
        text, err = sm4_source(text)
        if err:
            return err
    with open(fixed, "w", encoding="utf-8") as f:
        f.write(text)
    r = subprocess.run([FXC, "/nologo", "/T", f"{stage}_{model}", "/E", "main", "/O3", "/DSVR_D3D11",
                        f"/DSVR_SPEC_CONSTANTS={mask}", "/I", os.path.dirname(os.path.abspath(hlsl)),
                        "/Fo", dxbc, fixed],
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


def convert(xsc, out):
    name = output_name(xsc)
    stage = "ps" if name.endswith(".ps") else "vs"
    main_files = {"dxil": ".dxil", "spirv": ".spv", "dxbc": ".dxbc"}
    hlsl = os.path.join(out, "hlsl", name + ".hlsl")
    stale = False
    if stage == "vs" and os.path.exists(hlsl):
        # (converted before integer inputs could take floats - XMD_UINT_INPUT: again)
        old = open(hlsl, encoding="utf-8", errors="ignore").read()
        stale = ("in uint4 i" in old and "XMD_UINT_INPUT" not in old) or "precise float4 oPos" not in old
    if not stale and all(os.path.exists(os.path.join(out, d, name + main_files[d])) and
                         os.path.exists(os.path.join(out, d, name + ".textures")) for d in TARGETS):
        return name, None, ""
    r = subprocess.run([XR, xsc, hlsl, HEADER], capture_output=True, text=True, timeout=60)
    if r.returncode or not os.path.exists(hlsl) or not os.path.getsize(hlsl):
        return name, "hlsl", (r.stderr or r.stdout).strip()[:300]
    src = open(hlsl, encoding="utf-8", errors="ignore").read()
    # Some of this game's vertex shaders fetch one vertex element twice (two
    # fetch instructions with the same usage): one input, declared once.
    start = src.find("void main(")
    end = src.find("{", start)
    if start >= 0 and end > start:
        seen, kept = set(), []
        for line in src[start:end].split("\n"):
            m = INPUT_RE.search(line)
            if m and (m.group(3), m.group(4)) in seen:
                continue
            if m:
                seen.add((m.group(3), m.group(4)))
            kept.append(line)
        fixed = src[:start] + "\n".join(kept) + src[end:]
        if fixed != src:
            src = fixed
            with open(hlsl, "w", encoding="utf-8") as f:
                f.write(src)
    indexed = []
    if stage == "vs":
        fixed, indexed = rewrite_indexed(src, indexed_fetches(xsc))
        if fixed is None:
            return name, "indexed fetches", indexed
        if indexed:
            src = fixed
            with open(hlsl, "w", encoding="utf-8") as f:
                f.write(src)
    if stage == "vs" and "precise float4 oPos" not in src:
        # The position computed exactly as written (no fused or reordered arithmetic): the game draws its depth
        # pass and its colour pass with different shaders and tests the second against the first - a last-bit
        # difference left the ground black in patches.
        src = re.sub(r"\bout float4 oPos\b", "out precise float4 oPos", src)
        with open(hlsl, "w", encoding="utf-8") as f:
            f.write(src)
    if stage == "vs":
        # An integer input (BLENDINDICES as the converter declares it) that the game gives floats - a particle's
        # size: the renderer passes the floats' bits and says which inputs (the nth integer input: bit 16 + n of
        # g_SwappedTexcoords), the shader takes them as floats.
        start = src.find("void main(")
        open_at = src.find("{", start)
        names = re.findall(r"in uint4 (i\w+) :", src[start:open_at])
        if names and "XMD_UINT_INPUT" not in src:
            head, tail = src[:open_at], src[open_at:]
            for n, input_name in enumerate(names):
                tail = re.sub(r"\b%s\b" % input_name, f"XMD_UINT_INPUT({input_name}, {n})", tail)
            at = head.rfind("[shader(", 0, start)
            at = start if at < 0 else (head.rfind("#ifndef __spirv__", 0, at) if head.rfind("#ifndef __spirv__", 0, at) >= at - 24 else at)
            define = "#define XMD_UINT_INPUT(V, N) ((g_SwappedTexcoords & (0x10000u << (N))) != 0 ? asfloat(V) : float4(V))\n"
            src = head[:at] + define + head[at:] + tail
            with open(hlsl, "w", encoding="utf-8") as f:
                f.write(src)
    if stage == "ps":
        # The game draws its scene into 2:10:10:10 float targets, which hold red, green and blue from 0 to
        # 31.875 and alpha from 0 to 1. The renderer's targets for them are 16-bit float, which keep anything:
        # alpha above 1 or negative colours then went through blending and the fog and bloom passes (black
        # ground, flat fog, a white screen in the levels). Every pixel shader's outputs are held to that range
        # as it returns (its other targets are 8 bits a channel, which do it themselves).
        start = src.find("void main(")
        open_at = src.find("{", start)
        outputs = sorted(set(re.findall(r"out float4 (oC\d)\b", src[start:open_at])))
        if outputs and "XMD_HOLD_OUTPUTS" not in src:
            hold = " ".join(f"{o} = float4(clamp({o}.xyz, 0.0, 31.875), saturate({o}.w));" for o in outputs)
            if os.environ.get("XMD_SHADER_NAN_MAGENTA"):  # (test aid: a not-a-number or infinite colour shows magenta)
                hold = " ".join(f"if (any(isnan({o})) || any(isinf({o}))) {o} = float4(8.0, 0.0, 8.0, 1.0); else {{ {h}; }}" for o, h in zip(outputs, hold.split(";")[:-1]))
            head, tail = src[:open_at], src[open_at:]
            tail = re.sub(r"\breturn;", "{ XMD_HOLD_OUTPUTS return; }", tail)
            end = tail.rstrip().rfind("}")
            if not tail[:end].rstrip().endswith("return; }"):
                tail = tail[:end] + "\tXMD_HOLD_OUTPUTS\n" + tail[end:]
            at = head.rfind("[shader(", 0, start)
            at = start if at < 0 else (head.rfind("#ifndef __spirv__", 0, at) if head.rfind("#ifndef __spirv__", 0, at) >= at - 24 else at)
            src = head[:at] + f"#define XMD_HOLD_OUTPUTS {hold}\n" + head[at:] + tail
            with open(hlsl, "w", encoding="utf-8") as f:
                f.write(src)
    body = src[src.find("void main("):]
    variants = [0]
    if stage == "vs" and "tfetchR11G11B10(" in body:
        variants.append(1)   # packed 11:11:10
        variants.append(32)  # signed bytes
        variants.append(64)  # packed 10:11:11
    if stage == "ps" and "SPEC_CONSTANT_ALPHA_TEST" in body:
        variants.append(2)
    for mask in variants:
        suffix = "" if mask == 0 else f".s{mask}"
        if "dxil" in TARGETS:
            err = compile_dxil(hlsl, os.path.join(out, "dxil", f"{name}{suffix}.dxil"), stage, mask)
            if err:
                return name, f"dxil s{mask}", err
        if "spirv" in TARGETS:
            err = compile_spirv(hlsl, os.path.join(out, "spirv", f"{name}{suffix}.spv"), stage, mask)
            if err:
                return name, f"spirv s{mask}", err
            # (.spvc: small per-draw tables, for GPUs without descriptor indexing)
            err = compile_spirv(hlsl, os.path.join(out, "spirv", f"{name}{suffix}.spvc"), stage, mask, True)
            if err:
                return name, f"spirv compact s{mask}", err
        if FXC and "dxbc" in TARGETS:
            err = compile_dxbc(hlsl, os.path.join(out, "dxbc", f"{name}{suffix}.dxbc"), stage, mask,
                               os.path.join(out, "hlsl_d3d11"))
            if err:
                return name, f"dxbc s{mask}", err
            # (.dxbc4: Shader Model 4.0, for feature level 10_x GPUs)
            err = compile_dxbc(hlsl, os.path.join(out, "dxbc", f"{name}{suffix}.dxbc4"), stage, mask,
                               os.path.join(out, "hlsl_d3d11"), "4_0")
            if err:
                return name, f"dxbc4 s{mask}", err
    slots = {(name, dim): int(slot) for name, dim, slot in TEXTURE_DEFINE_RE.findall(src)}
    used = sorted({(slots[(name, dim)], DIMENSIONS[dim]) for name, dim in TEXTURE_USE_RE.findall(body)
                   if (name, dim) in slots})
    for d in sorted(TARGETS):
        with open(os.path.join(out, d, name + ".textures"), "w") as f:
            for slot, dim in used:
                f.write(f"{slot} {dim}\n")
        if stage == "vs":
            header = body[:body.find("{")]
            with open(os.path.join(out, d, name + ".inputs"), "w") as f:
                for location, typ, semantic, index in INPUT_RE.findall(header):
                    f.write(f"{semantic} {index} {typ} {location or -1}\n")
            # (the elements fetched by a computed index, in the order of their rows: SEMANTIC INDEX)
            with open(os.path.join(out, d, name + ".indexed"), "w") as f:
                for semantic, index in indexed:
                    f.write(f"{semantic} {index}\n")
    return name, None, ""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "port", "runs", "shaders_native")
    xsc_dirs = sys.argv[2:] or [os.path.join(ROOT, "port", "runs", "shaders_disc"),
                                os.path.join(ROOT, "port", "runs", "shaders")]
    for d in ("hlsl", "hlsl_d3d11", "dxil", "spirv", "dxbc"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    files = sorted(f for d in xsc_dirs for f in glob.glob(os.path.join(d, "*.xsc")))
    if os.environ.get("XMD_STAMP_ONLY"):  # (the side files only, from the HLSL already there)
        for f in files:
            if f.endswith(".vs.xsc"):
                stamp_inputs(f, out)
        counts = [stamp_consts(f, out) for f in files]
        known = [c for c in counts if c is not None]
        print(f"{len(known)} of {len(files)} .consts written; mean {sum(known) / max(1, len(known)):.1f} constants, "
              f"most {max(known, default=0)}")
        return
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("XMD_JOBS", "8"))) as pool:
        results = list(pool.map(lambda f: convert(f, out), files))
    for f in files:
        if f.endswith(".vs.xsc"):
            stamp_inputs(f, out)
        stamp_consts(f, out)
    # The renderer's own shaders (src/native/shaders/<name>.<vs|ps>.hlsl).
    own = os.path.join(ROOT, "port", "src", "native", "shaders")
    for hlsl in sorted(glob.glob(os.path.join(own, "*.hlsl"))):
        name = os.path.basename(hlsl)[:-5]
        if "dxil" in TARGETS:
            err = compile_dxil(hlsl, os.path.join(out, "dxil", name + ".dxil"), name.split(".")[-1], 0)
            if err:
                results.append((name, "own", err))
        # (the debug shaders but debug_solid are D3D12 only)
        for ext, compact in ((".spv", False), (".spvc", True)) if "spirv" in TARGETS else ():
            err = compile_spirv(hlsl, os.path.join(out, "spirv", name + ext), name.split(".")[-1], 0, compact)
            if err and (not name.startswith("debug_") or name.startswith("debug_solid")):
                results.append((name, "own spirv", err))
        if FXC and "dxbc" in TARGETS:
            for ext, model in ((".dxbc", "5_0"), (".dxbc4", "4_0")):
                err = compile_dxbc(hlsl, os.path.join(out, "dxbc", name + ext), name.split(".")[-1], 0,
                                   os.path.join(out, "hlsl_d3d11"), model)
                if err and (not name.startswith("debug_") or name.startswith("debug_solid")):
                    results.append((name, "own " + ext[1:], err))
    failed = [r for r in results if r[1]]
    with open(os.path.join(out, "report.txt"), "w") as rep:
        rep.write(f"{len(files)} shaders, {len(files) - len(failed)} converted ({', '.join(sorted(TARGETS))})\n")
        for name, stage, err in failed:
            rep.write(f"{name}: {stage}: {err}\n")
    vs = sum(1 for f in files if f.endswith(".vs.xsc"))
    print(f"{len(files)} shaders ({vs} vertex, {len(files) - vs} pixel): "
          f"{len(files) - len(failed)} converted ({', '.join(sorted(TARGETS))}), {len(failed)} failed -> {out}")


if __name__ == "__main__":
    main()
