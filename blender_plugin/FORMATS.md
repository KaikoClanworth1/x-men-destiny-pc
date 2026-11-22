# X-Men: Destiny (Xbox 360) data formats

Notes by KaikoClanworth, worked out for this importer. Everything is big-endian.
"Hash" always means Bob Jenkins' lookup3 (`hashlittle`, seed 0) of the upper-cased name.

## Archives (`name.zzz.gz`, `name_tex.gNN.gz`)

A gzip stream holding chunks back to back. Chunk header, 0x34 bytes:

| offset | meaning |
|---|---|
| 00 | magic `F00DCAFE 12344321` |
| 08 | size of the data that follows |
| 0C | class hash (kind of data) |
| 10, 14 | struct hash, struct size - set for "object" chunks (arrays of fixed structs) |
| 18 | name hash of the resource |
| 1C, 20 | streamed sizes (texture data) |
| 24, 28 | flags |
| 2C | element count |
| 30 | alignment |

`filetable.gz` maps the disc's shortened file names (`...#014.g05.gz`) to full names.

Chunk classes used by the importer:

| class | content |
|---|---|
| A7ADB1A0 | skinned model description (0x7C): name, bone count, skeleton id, stream ids |
| C10704A5 | static model description (0x7C) |
| 3CAFB711 | draw sections of the model with the same name hash |
| 07293B20 | vertex stream descriptions, 0x1C each |
| 78BD18B4 / FDD6F811 | vertex / index buffer bytes (named like the model) |
| 379E77D8 | index buffer description, 0x1C |
| F6F6F9E2 | skeleton (+ bone palette) |
| 6D2B6901 | generic container (`44556677`, version): sockets (v1), collision meshes (v7), zone info |
| 6A619BD7 | material instance (0xB0): textures and slot names |
| 2675F90C | material scalar parameters with their names |
| B78F9B0A, 0EB813D0, 616F3384, D35E7207 | shader set, pixel shader, vertex shader, constant table |
| 243D68CD / BB09D631 | texture header (0x48) / texture data |
| 37FA9D1B / D736E4F4 | full-size (streamed) texture header / data, in `_tex` archives |
| 4234DD6B / 1152C93A | animation clip header (0x30) / packed clip data |
| 8804E714 | placed objects of a level (0x14C each) |

## Models

Section (0x4C): `+04` flags, `+08` first index, `+0C` triangle count, `+10/+14` first/last
vertex, `+18` shader set, `+1C` material id, `+20` bone palette count, `+24` palette byte offset,
`+30` bounding box (6 floats). Triangles are clockwise lists of 16-bit indices.

Stream description (0x1C): id, 0, usage id, format, byte size, byte offset, data chunk name.

| format | bytes | content |
|---|---|---|
| 3 | 8 | position, 4 half floats |
| 4 | 12 | two tangent vectors and the normal (the third) - each 4 signed bytes stored `w z y x` |
| 7 | 4 | bone indices into the section's palette |
| 8 | 4 | bone weights /255 |
| 9 | 4 | vertex colour |
| D | 4 | UV, 2 half floats |
| E | 32 | static vertex: float3 position, normal 10:11:11 (x low 11 bits), tangent, binormal, half2 UV, half2 lightmap UV |

Skeleton: `44556677, 2, size, 0, id, bone count, 0x40, 0`, then per bone: rotation quaternion
(x y z w), position (3 floats + pad), name hash, parent index (own index = root), 16 zero bytes -
all relative to the parent. A bone palette follows: id, count, 2, 0, then 16-bit bone indices.
Bones are 3ds Max Biped bones (`Bip01 Pelvis`...); units are centimetres, Z up.

Material instance: `+00` id (also names the parameter chunk), `+1C` up to 16 texture name
hashes, `+5C` the matching slot name hashes (`diff01`, `spec1`, `norm2`, `mask`, `alpha`...),
last word: texture count in bits 16-23.

## Textures

Header (0x48): `+08` name hash, `+14/+18` width/height, `+20` D3D format word (low 6 bits:
0x12 DXT1, 0x13 DXT3, 0x14 DXT5, 0x06 A8R8G8B8, 0x02 L8), `+24` mip count, `+28` data size,
`+2C` data chunk name, `+44` size of the largest mip. Data is the usual Xbox 360 layout: tiled,
16-bit words byte-swapped, largest mip first. Normal maps are DXT5 with x in alpha and y in green.
A level archive holds only 16x16 stand-ins; the real textures are in `_tex` archives, which are
shared between levels (find them by texture name hash).

## Animation clips

Clip header (0x30): `+18` clip name hash, `+1C` data chunk name, `+20` offset, `+24` packed size,
`+28` unpacked size. The data is packed with byte pair encoding (blocks of: pair table, 16-bit
length, bytes).

Unpacked: `+00` name hash, `+0C` frames per second (30), `+10` frame count, `+14` track count,
`+18` root movement over the clip, `+34` one bone name hash per track, then the first pose
(26 bytes per track) and a stream of 16-bit words:

| word | followed by | meaning |
|---|---|---|
| `1nnn` | 3 half floats | position key of track nnn |
| `2nnn` | 4 half floats | rotation key (quaternion x y z w) |
| `3nnn` | 3 half floats | scale key |
| `4000` | frame number | the keys that follow belong to this frame |
| `5000` | frame number | playback position marker (keys are stored in the order playback needs them) |

Keys are sparse and relative to the parent bone. A track whose position keys are all zero is
not animated: its position is the one in the first pose. Track 0 is the root: its position keys
are the character's movement.

## Levels

Placed object (0x14C): `+00` id, `+40` scale, `+68` position, `+78` rotation quaternion,
`+B4` model name hash, `+E0` lightmap id, `+F4/+10C` lightmap scale/offset.
