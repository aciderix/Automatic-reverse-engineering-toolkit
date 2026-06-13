# Opal / Zouna (LyN) BigFile format — recovered spec

Reverse-engineered from *The Mighty Quest for Epic Loot* (engine
`Opal 2.0 BigFile | Data Version v0.288`), by reading the game loader
(`sub_9cb320`) and validating against 14 real `PACKAGE_*.BFPC` files
(69,092 nodes). Reader/extractor: `tools/bigfile/opal_bigfile.py`.

## Status

- **Container + node table: fully recovered and validated** on every package
  tested (tiny manifests to 100 MB+ asset packs). Universal extraction works.
- **Per-node-type sub-formats: partially recovered** (characterised; a few
  named). Decoding every type's internal layout is the remaining work.

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

## Tooling

```bash
opal_bigfile.py PACKAGE.BFPC                 # describe container + node tables
opal_bigfile.py PACKAGE.BFPC --extract out/  # one .node file per node + index
opal_bigfile.py <dir> --batch out/           # extract every bigfile in a dir
                                             # + global manifest.csv
```

## Remaining work — decoding all sub-formats

The scalable route (instead of guessing each type) is to recover the engine's
**node deserialiser dispatch**: the function that switches on `class_guid` and
calls a per-type reader. From the binary that gives, for every type:

1. its real name (the hashed type string), and
2. its field layout (the read sequence).

That converts the catalogue above into named, fully-decoded structures. Then
each `--extract`ed `.node` payload can be parsed into meshes/textures/objects.
