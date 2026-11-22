# X-Men: Destiny (Xbox 360) archive reader.  By KaikoClanworth
#
# A game archive (name.zzz.gz, name_tex.gNN.gz) is a gzip stream holding a flat
# list of chunks.  Every chunk has a 0x34-byte big-endian header:
#   +00 F00DCAFE 12344321   magic
#   +08 data size           bytes that follow the header
#   +0C class hash          what kind of data this is
#   +10 struct hash, +14 struct size   (only for "object" chunks: an array of structs)
#   +18 name hash           the resource's name (lookup3 hash of the upper-cased name)
#   +1C, +20                sizes used for streamed texture data
#   +24 flags, +28 ?, +2C element count, +30 alignment
import gzip
import os
import struct

MAGIC = b"\xf0\x0d\xca\xfe\x12\x34\x43\x21"

# chunk classes (hashes of names we do not know; labels are ours)
C_MODEL = 0xA7ADB1A0         # object: model description
C_SECTIONS = 0x3CAFB711      # draw sections of a model
C_STREAMS = 0x07293B20       # object: vertex stream descriptions
C_VERTEX_DATA = 0x78BD18B4   # vertex buffer bytes
C_INDEX_DESC = 0x379E77D8    # object: index buffer description
C_INDEX_DATA = 0xFDD6F811    # index buffer bytes
C_SKELETON = 0xF6F6F9E2
C_SOCKETS = 0x6D2B6901
C_MATERIAL = 0x6A619BD7      # object: material instance (textures)
C_MATERIAL_DEF = 0x2675F90C  # named scalar parameters
C_SHADER_SET = 0xB78F9B0A
C_TEXTURE = 0x243D68CD       # object: texture header
C_TEXTURE_DATA = 0xBB09D631
C_STATIC_MODEL = 0xC10704A5  # object: static (level) model description
C_INSTANCES = 0x8804E714     # object: placed models of a level archive
C_STREAM_TEXTURE = 0x37FA9D1B       # object: full-size texture header (in *_tex archives)
C_STREAM_TEXTURE_DATA = 0xD736E4F4
C_ANIM = 0x4234DD6B          # object: animation clip header
C_ANIM_DATA = 0x1152C93A


class Chunk:
    __slots__ = ("archive", "cls", "name", "size", "offset", "struct_hash",
                 "struct_size", "count", "a", "b", "flags")

    @property
    def data(self):
        return self.archive.data[self.offset:self.offset + self.size]

    def __repr__(self):
        return "<Chunk %08X name %08X size %d>" % (self.cls, self.name, self.size)


class Archive:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        if path.lower().endswith(".gz"):
            with gzip.open(path, "rb") as f:
                self.data = f.read()
        else:
            with open(path, "rb") as f:
                self.data = f.read()
        self.chunks = []
        self.by_class = {}
        self.by_key = {}
        d = self.data
        o = 0
        n = len(d)
        while o + 0x34 <= n:
            if d[o:o + 8] != MAGIC:
                break
            h = struct.unpack_from(">11I", d, o + 8)
            c = Chunk()
            c.archive = self
            c.size, c.cls, c.struct_hash, c.struct_size, c.name = h[0], h[1], h[2], h[3], h[4]
            c.a, c.b, c.flags, c.count = h[5], h[6], h[7], h[9]
            c.offset = o + 0x34
            self.chunks.append(c)
            self.by_class.setdefault(c.cls, []).append(c)
            self.by_key.setdefault((c.cls, c.name), c)
            o += 0x34 + c.size

    def all(self, cls):
        return self.by_class.get(cls, [])

    def get(self, cls, name):
        return self.by_key.get((cls, name))


class GameFiles:
    """The game's folder: finds archives by name and keeps a few open."""

    def __init__(self, folder):
        self.folder = folder
        self._names = None
        self._open = {}
        self._order = []
        self.max_open = 6

    def names(self):
        if self._names is None:
            self._names = {}
            for n in os.listdir(self.folder):
                low = n.lower()
                if low.endswith(".gz") and low not in ("filetable.gz", "filesizes.gz"):
                    self._names[low] = n
            # the disc shortens long names ("...#014.g05.gz"); filetable.gz maps them back
            ft = os.path.join(self.folder, "filetable.gz")
            self.long_names = {}
            if os.path.exists(ft):
                try:
                    for line in gzip.open(ft, "rt", errors="replace"):
                        parts = line.strip().split(",")
                        if len(parts) == 2:
                            self.long_names[parts[1].lower()] = parts[0]
                except OSError:
                    pass
        return self._names

    def find(self, name):
        """Full path of an archive by its (long or short) file name, or None."""
        names = self.names()
        low = name.lower()
        low = self.long_names.get(low, low).lower()
        real = names.get(low)
        return os.path.join(self.folder, real) if real else None

    def open(self, path):
        key = os.path.normcase(os.path.abspath(path))
        a = self._open.get(key)
        if a is None:
            a = Archive(path)
            self._open[key] = a
            self._order.append(key)
            while len(self._order) > self.max_open:
                self._open.pop(self._order.pop(0), None)
        return a

    def texture_archives(self, archive_path):
        """Archives that can hold the full-size textures of `archive_path`:
        name_tex.zzz.gz and name_tex.gNN.gz."""
        base = os.path.basename(archive_path).lower()
        for ext in (".zzz.gz", ".zzz"):
            if base.endswith(ext):
                base = base[:-len(ext)]
                break
        out = []
        names = self.names()
        long_to_short = self.long_names
        wanted = base + "_tex."
        for low, real in names.items():
            if low.startswith(wanted):
                out.append(os.path.join(self.folder, real))
        for long_name, short in long_to_short.items():
            if long_name.startswith(wanted) and short.lower() in names:
                out.append(os.path.join(self.folder, names[short.lower()]))
        return sorted(set(out))

    # ---- full-size (streamed) textures live in *_tex archives, shared between levels

    def stream_index(self, cache_file=None, progress=None):
        """{texture name hash: archive file name} for every *_tex archive (cached on disk)."""
        if getattr(self, "_stream_index", None) is not None:
            return self._stream_index
        import json
        names = self.names()
        tex = sorted(real for low, real in names.items() if "_tex." in low or "#" in low)
        stamp = [[n, os.path.getsize(os.path.join(self.folder, n))] for n in tex]
        if cache_file and os.path.exists(cache_file):
            try:
                with open(cache_file, "r") as f:
                    saved = json.load(f)
                if saved.get("stamp") == stamp:
                    self._stream_index = {int(k): v for k, v in saved["index"].items()}
                    return self._stream_index
            except (OSError, ValueError, KeyError):
                pass
        index = {}
        for i, n in enumerate(tex):
            if progress:
                progress(i, len(tex), n)
            try:
                a = Archive(os.path.join(self.folder, n))
            except (OSError, EOFError):
                continue
            for c in a.all(C_STREAM_TEXTURE):
                index.setdefault(c.name, n)
        self._stream_index = index
        if cache_file:
            try:
                with open(cache_file, "w") as f:
                    json.dump({"stamp": stamp, "index": {str(k): v for k, v in index.items()}}, f)
            except OSError:
                pass
        return index
