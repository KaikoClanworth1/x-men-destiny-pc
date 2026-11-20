"""Writes the Lua mission-script text found in the level archives (Game Files\*.zzz.gz) to runs\lua\<archive>.lua
- the scripts are plain text between binary data. For looking up actor IDs, event and flag names to use with the
debug command file (src\debug_commands.cpp: lua / teleport / event lines).
  python tools\lua_scripts.py
"""
import glob, gzip, os, re, sys
port = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
game = os.path.join(os.path.dirname(port), "Game Files")
out = os.path.join(port, "runs", "lua")
os.makedirs(out, exist_ok=True)
text = re.compile(rb"[\x09\x0a\x0d\x20-\x7e]{24,}")
word = re.compile(rb"\b(local|function|end|SetLuaVariableToActorUID|LuaWaitEvent|if|then)\b|[A-Za-z_]\w*\s*\(")
n = 0
for f in sorted(glob.glob(os.path.join(game, "*.zzz.gz"))):
    if "_tex" in os.path.basename(f): continue
    try: d = gzip.open(f).read()
    except Exception as e:
        print("unreadable:", f, e); continue
    parts = [m.group(0) for m in text.finditer(d) if len(word.findall(m.group(0))) >= 2]
    if not parts: continue
    name = os.path.basename(f)[:-7] + ".lua"
    with open(os.path.join(out, name), "wb") as o:
        o.write(b"\n-- ----\n".join(p.replace(b"\r\n", b"\n") for p in parts))
    n += 1
print(n, "files in", out)
