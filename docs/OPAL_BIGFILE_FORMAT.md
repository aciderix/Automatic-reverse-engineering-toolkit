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

### Texture_Z (image form) — recovered & validated (411 textures)

```
0x00 u64 instance id
0x0a u8  mip count
0x0d u32 width
0x11 u32 height
0x15 u32 data size   (always == payload_len - 0x26)
0x19 u32 format enum (observed 8, 12, 14 — BC/DXT family)
0x26 ...  pixel data, wrapped in the engine "Hx" codec (magic 48 78 00 ..)
```

Validated: dims are exact and sensible across every sample (1920×1080,
1280×720, 256×256, 760×240, …). The pixel payload is smaller than raw BC data
(e.g. 1920×1080 in 417 KB), so **"Hx" is a compression layer** — decoding it to
BC/raw pixels (then DDS/PNG) is the remaining texture step. `opal_nodes.py
--textures` extracts each texture's metadata + raw "Hx" blob now.
Texture_Z also has a small **descriptor form** (references the image node by
byte-reversed node_id + UV/colour scale floats) — `parse_texture` returns None
for it.

Other types: **Material_Z** (~82 B) embeds u64 references to its
Texture_Z/Shader_Z at `[0x24]`/`[0x4A]`; **Node_Z** (~175 B) embeds
child/transform references around `[0x42]` (recovered automatically by the
reference graph in `opal_nodes.py`).

## Tooling

```bash
opal_bigfile.py PACKAGE.BFPC                 # describe container + node tables
opal_bigfile.py PACKAGE.BFPC --extract out/  # one .node file per node + index
opal_bigfile.py <dir> --batch out/           # extract every bigfile in a dir
                                             # + global manifest.csv (named types)
opal_nodes.py out/                           # common-header decode + reference
                                             # graph (Material->Texture/Shader…)
opal_nodes.py out/ --graph deps.csv          # write the node dependency graph
```

## Remaining work — full per-type field layouts

### "Hx" texture codec (the Texture_Z pixel container)

The pixel blob inside Texture_Z is an **"Hx" container, big-endian** (recovered
with the SSA decompiler — `--mode ir` — which read it cleanly where the textual
output was ambiguous). Header layout (from parser `sub_959290`, BE-u32 reader
`sub_9584f0`, validator `sub_959220`):

```
0x00  "Hx"  magic (0x48 0x78)
0x02  u8    version/variant (>= 0x4a)
0x06  u8    high byte of a size, checked against the blob length
0x0c  u16BE width
0x0e  u16BE height
0x10  u8    (1)
0x11  u8    (1)
0x12  u8    pixel format: 9 => BC1/DXT1 (8-byte blocks); else BC3/DXT5
            (16-byte blocks). Observed value 2 => DXT5.
0x14+ big-endian per-mip size fields, then the (further-compressed) block data
```

The block data is **not raw DXT and not zlib/deflate/lz4** — it is a custom
**LZ + Huffman** codec (recovered with the SSA decompiler). `opal_nodes.py
--textures` already extracts the Hx blob + width/height/format metadata; what
remains is reimplementing this codec.

#### Hx codec internals (recovered)

- **Bit reader** `sub_95a240(state, n)` — MSB-first. State struct:
  `+0x04` read ptr, `+0x08` end ptr, `+0x10` bit buffer (32-bit, left-aligned),
  `+0x14` valid-bit count. Refills a byte at a time (`buf |= byte <<
  (32-(bits+8))`, `bits += 8`) until `bits >= n`, returns `buf >> (32-n)`, then
  consumes n bits (`bits -= n`, `buf <<= n`). Byte stream is consumed forward.
- **Core decoder** `sub_95a020` — a **custom DEFLATE variant** (LZ77 +
  canonical Huffman, table size `0x2000` = 2^13 max code length). It first
  reads the Huffman **code-length table** with a deflate-style RLE alphabet
  (one symbol decoded per `sub_959480` call):
    - symbol `<= 0x10`  : a literal code length (0..16)
    - symbol `0x11` (17): read **3** extra bits, value `+3`   (repeat)
    - symbol `0x12` (18): read **7** extra bits, value `+11`  (repeat)
    - symbol `0x13` (19): read **2** extra bits, value `+3`
    - symbol `0x14` (20): read **6** extra bits, value `+7`
  then decodes literals / LZ back-references with the built tables (copy
  helpers `sub_95a8f0`, `sub_d7a82e`). Drivers: `sub_95a780` -> `sub_95a020`;
  symbol decode `sub_959480`; setup `sub_95a890` / `sub_958480`.
- **Channel/mode decoders** `sub_959fb0` -> `sub_959590` / `sub_9596d0` /
  `sub_959a20` / `sub_959ca0` (per pixel-channel or per-mode passes).

**Confirmed: it is a DEFLATE variant.** The code-length code order table at
`.rdata 0xf3ce98` is
`[17,18,19,20, 0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15,16]` — DEFLATE's
code-length permutation, extended with RLE symbols 17–20. So a standard inflate
can be adapted with these differences:

- **Bit order**: MSB-first (not DEFLATE's LSB-first) — see `sub_95a240`.
- **Code-length alphabet**: 21 symbols read (3 bits each) in the order above to
  build the code-length Huffman table.
- **Code-length RLE** (decoding the main alphabet's lengths):
  17→+3 (3 extra bits), 18→+11 (7 bits), 19→+3 (2 bits), 20→+7 (6 bits).
- **Huffman**: canonical, decoded by `sub_959480` (fast table @struct+0xa8,
  symbol table u16 @+0xb0); built by `sub_95a890` → `sub_95a3d0`.
- **Channels**: `sub_95a780` runs the decoder (`sub_95a020`) once per plane
  (counts at Hx `+0x27`/`+0x37`), writing each channel's bytes; planes are then
  interleaved into the BC1/BC3 surface.

#### Full codec architecture (recovered end-to-end via SSA + asm)

The Hx codec is a **custom per-component DXT/BC transcoder**: each BC block
component is stored in its own sub-stream, Huffman-coded with a per-stream
DEFLATE-dynamic table, then **delta-decoded** (running sum mod 256). It is NOT
LZ — the earlier "LZ" read was the dynamic-table RLE, not the data.

Pipeline: `sub_95a380` = validate (`sub_959220`) → build tables (`sub_95a780`,
up to 5× `sub_95a020`) → decode components (`sub_959fb0`).

**Table builder `sub_95a020`** (DEFLATE-dynamic header, MSB-first):
```
N      = read_bits(14)                 # symbol-count (HLIT), max 0x2000
alloc code_lengths[N]
M      = read_bits(5)                  # code-length-code count (HCLEN), <=21
for i in 0..M:  meta_len[PERM[i]] = read_bits(3)   # PERM = table @0xf3ce98
build canonical meta-table
decode N code lengths via meta-table with RLE: 17→+3(3b) 18→+11(7b) 19→+3(2b) 20→+7(6b)
build canonical main table   (sub_95a890 -> sub_95a3d0)
```
Canonical symbol decode = `sub_959480` (fast table @+0xa8, u16 symbol table @+0xb0).

**Component decoders** (`sub_959fb0` runs them; gated by header counts @0x27 / @0x37):
each reads `(offset, size, count)` as **big-endian** from the header, sets a
bitreader over `[base+offset, +size)`, builds a table with `sub_95a020`, then
delta-decodes `count` entries:

| decoder      | header fields (off/size/count) | output | BC role |
|--------------|--------------------------------|--------|---------|
| `sub_959590` | be24@0x31 / be24@0x34 / be16@0x37 | `count` u16, two delta bytes each (1 table) | colour endpoints |
| `sub_9596d0` | be24@0x39 / be24@0x3c / be16@0x3f | 3-bit packed (remap table @0xf3ce80, shl 3) | colour indices |
| `sub_959a20` | be24@0x21 / be24@0x24 / be16@0x27 | **2 tables**; per entry 6 deltas, masks 5-bit (`&0x1f`) / 6-bit (`&0x3f`), packed shl 5/6 | alpha indices |
| `sub_959ca0` | (its own @0x29.. block)           | delta bytes | alpha endpoints |

Verified working: `build_table` reproduces all four sub-streams' tables, and
`sub_959590` (colour endpoints) decodes its full entry count consuming its
sub-block to the byte (`tools/bigfile/hx_codec.py`). `sub_959a20` uses two
tables and a 6-value mixed-mask delta pattern (above). Counts (e.g. 36/41/47/34
for a 32×32) are below the 64 BC blocks, so a palette + per-block index
indirection maps decoded entries to blocks.

Decoder complexity varies sharply:
- `sub_959590` (colour endpoints): simple 1-table 2-byte delta — **done**.
- `sub_959a20` (alpha indices): 2 tables, 6 mixed-mask deltas — mapped.
- `sub_9596d0` (colour indices): a **2-D predictive delta coder** (intricate
  but tractable, NOT an opaque arithmetic model). Closer reading:
  1. A 15×15 offset grid (`[ebp-0x3e4]` = dx, `[ebp-0x768]` = dy, each in
     [-7,7]) is built once; a Huffman symbol indexes it to a `(dx,dy)`.
  2. Per block: decode 8 Huffman symbols; each advances 8 running `(x,y)`
     states, accumulated **mod 8** (`& 7`).
  3. Each state is remapped through table `0xf3ce80`
     (`[0,2,3,4,5,6,7,1, 1,0,5,4,3,2,6,7, …]`) and packed as 3-bit fields
     (`shl 3`) into u16 words = the block's colour indices.

**Bottom line**: the codec is conceptually fully reverse-engineered. The
entropy core (bit reader, dynamic Huffman, table builder, delta channels) is
validated working code (`hx_codec.py`); colour-endpoints decode exactly. What
remains is intricate-but-deterministic implementation: the colour-index 2-D
delta packing, `sub_959ca0` (alpha endpoints), the palette→block index
indirection, and BC interleaving → DDS. The improved SSA decompiler chiefly
speeds up nailing the exact masks/shifts of the packing — the scheme is known.

Delta decode (per `sub_959590`): `acc = (acc + huff_sym()) & 0xff` for each
byte; two bytes packed per u16 entry.

**Remaining = implementation only** (the reverse engineering is complete):
port the MSB-first bit reader + canonical Huffman + this dynamic-table builder +
the four delta decoders, interleave the components into BC1/BC3 blocks per mip,
validate against `width*height*blockBytes/16`, and DDS-wrap. This is a sizeable
but mechanical build; the improved SSA decompiler makes the four component
decoders' exact bit-packing fastest to finalise.

### Other remaining layouts
- **Mesh_Z / Skin_Z / SkinData_Z**: vertex/index buffer layout (→ OBJ/glTF).
- Field layouts of the rarer types (Conductor_Z, Override_Z, SpecialEffectNode_Z…).

Each type reader is reached through the runtime registry (`sub_96a060` →
factory → reader). The SSA/IR decompiler (`--mode ir`) is the right tool for
reading these: it folds the big-endian/bit-twiddling code into readable form.
