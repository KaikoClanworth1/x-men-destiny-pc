# X-Men: Destiny (Xbox 360) importer for Blender

By KaikoClanworth

A Blender add-on that reads the archives of **your own copy** of X-Men: Destiny (Xbox 360)
and imports:

1. **Models** - characters and props with mesh, skeleton, skin weights, UVs, normals and
   node materials whose textures are found automatically in the game folder.
2. **Animations** - clips as actions on an imported armature (30 fps, root motion optional).
3. **Maps** - a level archive as a scene: its static models, placed, rotated and scaled as the
   level's placement table says, with materials.

The add-on contains no game data. It needs the extracted disc: the folder that holds the
`*.zzz.gz` files (in the PC port this is the `Game Files` folder).

## Install

1. Build the zip (or take a released one): `python build_zip.py` writes `dist/io_xmd-<version>.zip`.
2. Blender: *Edit > Preferences > Get Extensions >* (arrow, top right) *> Install from Disk...*,
   pick the zip. The add-on is enabled on install.

Written for and tested with Blender 5.2. The manifest allows 4.2 and later, but older versions
were not tested.

## Use

**File > Import** has three entries; each opens a file browser on the game folder:

| Entry | Pick | Result |
|---|---|---|
| X-Men: Destiny model | a character/prop archive, e.g. `xmen008.zzz.gz` (Cyclops), `xmen009.zzz.gz` (Wolverine), `pla001_wolverine.zzz.gz` (a hero in a suit) | mesh + armature + materials |
| X-Men: Destiny animations | an archive with clips, e.g. the character's own archive or `pla001.zzz.gz` - with the imported armature selected | actions (`archive_NNN_HASH`), the first one made active |
| X-Men: Destiny map | one or more level archives, e.g. `xmd_ferrybuilding_zone01_art.zzz.gz` | one collection per archive |

The **XMD** tab of the 3D view's sidebar (press N) is a small browser: choose the game folder,
*List archives*, select one, then *Import model*, *Import as map* or *Import animations*
(onto the active armature); *Previous clip / Next clip* steps through imported actions.

Things to know:

- The game works in centimetres, Z up. The default scale 0.01 gives metres.
- The first map import builds an index of the game's texture archives (a minute or so); it is
  kept in Blender's config folder (`xmd_importer/texture_index.json`) and reused.
- A level is split over many archives (`..._zone01_art`, `..._zone02_art`, ...), all in the same
  world coordinates: import several to assemble a level. Their `_tex` archives are found
  automatically - do not pick those.
- Names in the game are stored only as hashes. Bones get their standard Biped names
  (`Bip01 L Thigh`...) where the hash matches, otherwise `bone_XXXXXXXX`; clips and models are
  named by archive and hash. Every bone, object, action and texture node carries its hash
  (custom properties `xmd_*`, node labels), so nothing is lost for writing data back later.
- Players: `plaNNN_<suit>.zzz.gz` is hero N wearing a suit, `plaNNN.zzz.gz` holds that hero's
  animations. NPCs are `xmenNNN`, `magneto`, `gambit`, `juggernaut`...
- Every texture a material uses is added as an image node (labelled with its slot name);
  diffuse, normal, specular, emissive and alpha are connected to a Principled BSDF.

## Not supported / known gaps

- Import only - nothing is written back to the game.
- Materials are an approximation: the game's own shaders (layer blending by masks, tint
  parameters, rim light, reflections) are not rebuilt. Blended layers (`diff02`, `norm2`...)
  and masks are loaded but left unconnected; the scalar parameters are stored as custom
  properties on the material. Some foliage/alpha cards come out with the wrong transparency.
- No lights: level lighting is baked into lightmaps (the second UV set is imported as
  `UVMap2`, the lightmaps themselves are not applied). Smoke/fog effect meshes are imported
  nearly transparent.
- No collision meshes, cameras, cut-scene timelines, particle effects, sound or scripts.
- Skinned props placed in levels and objects placed by mission scripts are not placed;
  *Also models without a placement* imports them at the origin.
- Facial and extra bones without a Biped name keep hash names. Clip names are hashes.
- Animation keys are imported as the game stores them (sparse keys, linear interpolation);
  bone scale tracks are only imported when they are not 1.

See `FORMATS.md` for the file formats as far as they are understood.

## Tests

`tests/` holds headless check scripts (not part of the zip), e.g.

    blender --background --factory-startup --python tests/test_model.py -- "<game folder>" xmen008.zzz.gz out.png

`tests/test_install.py` installs the zip into a throw-away profile (set `BLENDER_USER_RESOURCES`
to an empty folder first) and runs the three importers through their menu operators.
