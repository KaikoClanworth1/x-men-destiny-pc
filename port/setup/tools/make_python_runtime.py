"""Make a small stand-alone Python (the layout of python.org's "embeddable package") from the python.org install
that runs this script: the interpreter, its DLL, the few extension modules the shader tools use and the standard
library as one zip. The XMD Setup package carries it (sdk/shadertools/python) to run the port's shader tools on the
player's PC; nothing is downloaded.

    <python.org python.exe> make_python_runtime.py <out folder>
"""
import os
import shutil
import sys
import zipfile

# Standard library folders the tools never import (they are most of its size).
LEFT_OUT = {"site-packages", "test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "venv",
            "pydoc_data", "distutils", "unittest", "msilib", "curses", "sqlite3", "dbm", "wsgiref", "xmlrpc",
            "__pycache__", "idle_test"}
# Extension modules: ctypes (Windows' AES for the game program's image), and what the standard library's
# subprocess / threading / gzip paths may load.
MODULES = ["_ctypes.pyd", "libffi-8.dll", "_queue.pyd", "select.pyd", "_socket.pyd", "_bz2.pyd", "_lzma.pyd"]


def main():
    out = os.path.abspath(sys.argv[1])
    base = sys.base_prefix
    if "WindowsApps" in base or not os.path.exists(os.path.join(base, "LICENSE.txt")):
        sys.exit("this needs a python.org install of Python (not the Microsoft Store one): " + base)
    tag = "python%d%d" % sys.version_info[:2]
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for name in ("python.exe", "python3.dll", tag + ".dll", "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"):
        shutil.copy2(os.path.join(base, name), out)
    for name in MODULES:
        src = os.path.join(base, "DLLs", name)
        if os.path.exists(src):
            shutil.copy2(src, out)
    lib = os.path.join(base, "Lib")
    count = 0
    with zipfile.ZipFile(os.path.join(out, tag + ".zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(lib):
            dirs[:] = sorted(d for d in dirs if d not in LEFT_OUT)
            for f in sorted(files):
                if f.endswith(".py"):
                    path = os.path.join(root, f)
                    z.write(path, os.path.relpath(path, lib))
                    count += 1
    # Isolated: only its own zip and folder are searched (no registry, no environment, no user site).
    with open(os.path.join(out, tag + "._pth"), "w") as f:
        f.write(tag + ".zip\n.\n")
    size = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
    print("%s %s: %d library files, %.1f MB in %s" % (tag, sys.version.split()[0], count, size / 2 ** 20, out))


if __name__ == "__main__":
    main()
