# Opal / Zouna (LyN) BigFile format — recovered spec

Reverse-engineered from *The Mighty Quest for Epic Loot* (engine
`Opal 2.0 BigFile | Data Version v0.288`), by reading the game loader
(`sub_9cb320`) and validating against 14 real `PACKAGE_*.BFPC` files
(69,092 nodes). Reader/extractor: `tools/bigfile/opal_bigfile.py`.

## Status

- **Container + node table: fully recovered and validated** on every package
  tested (tiny manifests to 100 MB+ asset packs). Universal extraction works.
- **Node type system: fully recovered.** The type-id hash is broken (CRC-64),
  so every node's `class_guid` is mapped to its engine type name with zero
  collisions (see below).
- **Per-node-type sub-formats: partially recovered** (common object header +
  several types characterised). Decoding each type's full internal layout is
  the remaining work.

## Container layout

```
0x000  char header[256]   "Opal 2.0 BigFile | Data Version v… |" (zero-padded).
                          The version check that logs "Wrong version of bigFile
                          detected!" reads this string.
0x100  binary header
         u32 dir_ptr      offset (relative to 0x800) of the main node table
         u32 _
         u32 count        top-level section/node count
         u32 dir_ptr      (repeated)
         u32 dir_ptr      (repeated)
0x800  node table(s)      one or more 0xDEADBEEF chunks (see below)
```

A file contains one or more **node tables**, each introduced by the magic
`DE AD BE EF` followed by a node count.

## Node table chunk

```
u32  magic = 0xDEADBEEF        (stored as bytes DE AD BE EF)
u32  count                     number of node records that follow
…    count × node records …
```

## Node record (validated: tiles every chunk exactly)

```
u32   size           total block length, this 24-byte header included
u64   class_guid     node type id — a runtime hash of the type name, NOT a
                     literal in the exe (so types are named via the engine's
                     deserialiser dispatch, not a constant table)
u64   node_id        the ">NODE_xxx" identity of this asset
char  tag[4]         "_OBJ"
…     payload         size - 24 bytes, type-specific
```

Records are walked by following `size`; the next record begins immediately
after. Payloads are **not container-encrypted**; high whole-file entropy in
some packages is just already-compressed media (textures/sounds), not an
encrypted container.

## Node type catalogue (observed across 14 packages)

Class GUIDs are hashes of engine type names (cf. RTTI: `Mesh_Z`, `Texture_Z`,
`Shader_Z`, `Skin_Z`, `Particles_Z`, …). Characterisation so far:

| class_guid          | count | payload         | likely type |
|---------------------|-------|-----------------|-------------|
| `0ffaf91de7415fff`  | 28926 | fixed 183 B     | scene object / transform |
| `ffce5dbff83ac15d`  | 13247 | fixed 175 B     | scene object |
| `7c77b45d80201faf`  | 10052 | variable 7–58 KB| mesh / geometry data |
| `f0c9901ff0856878`  |  3342 | fixed 82 B      | small component |
| `e9fd6fb3d594e145`  |  1543 | 32 KB–524 KB    | texture (dims/mips header) |
| `677edd91a9ed0be9`  |    68 | —               | **shader** (embeds `DB:>RAW>SHADERS>…`) |

Fixed-size types whose payload begins `<8-byte id> 00… 0x3f800000` (a `1.0f`)
are engine scene objects (the float is an identity scale/quaternion term).

## Type id hash (recovered)

`class_guid = CRC-64/ECMA-182( type_name.upper() )` — polynomial
`0x42F0E1EBA9EA3693`, init/xorout = `0xFFFFFFFFFFFFFFFF`, non-reflected
(MSB-first), case-insensitive. Engine routine: `sub_9ce4b0` (tables at
`.data` VA `0x1060a50` CRC-64 table, `0x1061250` an a→A…z→Z fold table).
Implemented in `opal_bigfile.opal_type_id`; reproduces **all 39** observed
guids with no collisions.

### guid → type name (observed across 14 packages)

XRefNode_Z, Node_Z, Sound_Z, Material_Z, ParticlesData_Z, Particles_Z,
Texture_Z, Mesh_Z, SkinData_Z, Animation_Z, SpecialEffectNode_Z, Skin_Z,
EntityData_Z, TriggerNode_Z, Override_Z, Conductor_Z, Entity_Z, UIMaterial_Z,
ProjectorData_Z, Projector_Z, UIPanel_Z, AmbientLightmap_Z, Package_Z,
LensFlareData_Z, LensFlare_Z, UIFont_Z, UINineSlice_Z, UIContainer_Z,
UITextPanel_Z, SoundNode_Z, Camera_Z, EmbeddedFile_Z, SkelData_Z, Shader_Z,
Skel_Z, OmniData_Z, Omni_Z, AnimationCollection_Z, Object_Z.

(The engine registers ~120 types total — World_Z, BaseObject_Z, Game_Z,
Points_Z, ModifierContainer_Z, … — hashable the same way.)

## Dispatch / type registry (recovered)

- Each type has a lazy-init **TypeInfo singleton** (guid getter thunks in the
  CRT-style init array at VA `0xdf1b00`; thunks `0xde0670–0xde2000`).
- The global type/factory **registry object** is at `0x10d5538`.
- **`sub_96a060` / `sub_969fa0`** is the registry lookup: a GUID-keyed hash map
  (bucket index from the TypeInfo, linked-list walk via `next = *(entry+0xc)`)
  returning the registered factory/reader entry.
- Stream read primitives: `sub_9e75d0` / `sub_9e7440` / `sub_9e74c0`.

## Common node header (Object_Z base — every node payload)

```
[0x00:0x08]  u64  per-instance unique id
[0x08:0x16]       base fields (mostly zero)
[0x16:0x1A]  f32  base scalar: 1.0 for plain ref types; real value for spatial
                  types (Entity_Z, Omni_Z, Mesh_Z, Skin_Z, Projector_Z)
[0x1A:…]          per-type flags + data
```

Type notes: **Texture_Z** has a small descriptor form (~78 B, references the
big data node by byte-reversed node_id + UV/colour scale floats) and a large
form (mip-count byte at `[0x0a]`, then width/height/format words and per-mip
sizes, then BC/DXT pixel data). **Material_Z** (~82 B) embeds u64 references to
its Texture_Z/Shader_Z at `[0x24]` and `[0x4A]`. **Node_Z** (~175 B) embeds
child/transform references around `[0x42]`.

## Tooling

```bash
opal_bigfile.py PACKAGE.BFPC                 # describe container + node tables
opal_bigfile.py PACKAGE.BFPC --extract out/  # one .node file per node + index
opal_bigfile.py <dir> --batch out/           # extract every bigfile in a dir
                                             # + global manifest.csv
```

## Remaining work — full per-type field layouts

The type system and dispatch are recovered; what remains is the exact field
layout of each type's reader beyond the common header — notably:

- **Texture_Z** large form: byte-locking width/height/format-enum/mip-size
  table (start from the `0x10da1c8` Texture_Z TypeInfo callers `sub_9c83d0`,
  `sub_9e3870`, `sub_9fc6a0`, `sub_9fd4b0`), so pixel data can be exported
  (e.g. to DDS).
- **Mesh_Z / Skin_Z / SkinData_Z**: vertex/index buffer layout.
- The rarer types (Conductor_Z, Override_Z, SpecialEffectNode_Z, UI*).

Each reader is reached through the runtime registry (`sub_96a060` → factory →
reader), so mapping each TypeInfo singleton to its reader VA is the next step.
With the layouts, each `--extract`ed `.node` becomes a fully parsed asset.
