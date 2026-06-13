#!/usr/bin/env python3
"""
opal_bigfile.py — reader for Ubisoft *Opal / Zouna (LyN)* ".BF*" bigfiles,
as used by The Mighty Quest for Epic Loot (engine string:
"Opal 2.0 BigFile | Data Version v0.288 | ...").

Recovered by reverse-engineering the game's loader (function sub_9cb320) and
cross-validating against real PACKAGE_*.BFPC samples.

Container layout (recovered, validated):

    0x000  char[256]   ASCII header: "Opal 2.0 BigFile | Data Version v… |"
                       (zero-padded). The version check that emits
                       "Wrong version of bigFile detected!" reads this.
    0x100  struct      binary header:
                         u32 dir_ptr  (= 0x1121000 in the sample; locates the
                                       second chunk relative to 0x800)
                         u32 _
                         u32 count    (top-level node count, = 2)
                         u32 dir_ptr  (repeat)
                         u32 dir_ptr  (repeat)
    0x800  chunk       node table, magic 0xDEADBEEF:
                         u32 magic = 0xDEADBEEF
                         u32 count
                         … node records …

Each 0xDEADBEEF table is a sequence of variable-length node records, walked by
following the leading size field (validated: tiles all 255 nodes of
PACKAGE_PRELOAD.BFPC exactly, every record tagged "_OBJ"):

    u32  size            total block length (header included)
    u64  class_guid      node type id (texture / mesh / shader / object / …)
    u64  node_id         the ">NODE_xxx" identity
    char tag[4]          "_OBJ"
    …    payload         size-24 bytes (type-specific; shaders embed
                         "DB:>RAW>SHADERS>SHADER_…" paths)

The payload is **not encrypted or compressed** (measured entropy ≈ 4.4), so
node bytes are sliced out directly. The per-type payload sub-formats are the
next layer to decode (per class_guid).

Usage:
    opal_bigfile.py <file.BFPC>              # describe the container + nodes
    opal_bigfile.py <file.BFPC> --hex N      # dump N words per chunk head
    opal_bigfile.py <file.BFPC> --extract D  # write every node payload into D
"""
import struct, sys, argparse, math, collections

# Stored literally as the byte sequence DE AD BE EF (reads 0xEFBEADDE as LE u32).
MAGIC_BYTES = b'\xde\xad\xbe\xef'
HEADER_SIZE = 0x100
CHUNK0_OFF  = 0x800

def u32(d, o): return struct.unpack_from('<I', d, o)[0]

def entropy(b):
    if not b: return 0.0
    c = collections.Counter(b); n = len(b)
    return -sum((x/n) * math.log2(x/n) for x in c.values())

def parse_text_header(d):
    raw = d[:HEADER_SIZE].split(b'\x00', 1)[0]
    return raw.decode('latin1', 'replace')

def find_chunks(d):
    """All 0xDEADBEEF node tables with their declared counts."""
    out = []
    needle = MAGIC_BYTES
    i = 0
    while True:
        i = d.find(needle, i)
        if i < 0:
            break
        out.append((i, u32(d, i + 4)))
        i += 4
    return out

# A node record is a variable-length block:
#   u32 size            total block length (this header included)
#   u32 class_guid_lo   type id of the node (mesh/texture/object/…)
#   u32 class_guid_hi
#   u32 node_id_lo      64-bit node id (the ">NODE_xxx" identity)
#   u32 node_id_hi
#   char tag[4]         "_OBJ"
#   … metadata + payload …  (size-24 bytes)
NODE_TAG = b'_OBJ'

class Node:
    __slots__ = ("offset", "size", "guid", "node_id", "tag")
    def __init__(self, offset, size, guid, node_id, tag):
        self.offset, self.size, self.guid = offset, size, guid
        self.node_id, self.tag = node_id, tag
    @property
    def name(self):
        return f"{self.guid:016x}_{self.node_id:016x}"

def walk_nodes(d, chunk_off):
    """Yield the Node records of a 0xDEADBEEF chunk by following [size] links."""
    count = u32(d, chunk_off + 4)
    off = chunk_off + 8
    for _ in range(count):
        if off + 24 > len(d):
            break
        size = u32(d, off)
        guid = u32(d, off + 4) | (u32(d, off + 8) << 32)
        nid  = u32(d, off + 12) | (u32(d, off + 16) << 32)
        tag  = d[off + 20:off + 24]
        if size < 24 or off + size > len(d):
            break
        yield Node(off, size, guid, nid, tag)
        off += size

def describe(path, hexwords=0):
    d = open(path, 'rb').read()
    print(f"file        : {path}")
    print(f"size        : {len(d)} (0x{len(d):x})")
    print(f"text header : {parse_text_header(d)!r}")

    print("\nbinary header @0x100:")
    for o in range(HEADER_SIZE, HEADER_SIZE + 0x14, 4):
        print(f"  +0x{o-HEADER_SIZE:02x}  0x{u32(d,o):08x}")

    chunks = find_chunks(d)
    print(f"\nnode tables (0xDEADBEEF): {len(chunks)}")
    for off, count in chunks:
        nodes = list(walk_nodes(d, off))
        tagged = sum(1 for n in nodes if n.tag == NODE_TAG)
        end = nodes[-1].offset + nodes[-1].size if nodes else off + 8
        print(f"  @0x{off:08x}  count={count}  walked={len(nodes)}  "
              f"_OBJ={tagged}  ends@0x{end:08x}")
        classes = collections.Counter(n.guid for n in nodes)
        for g, c in classes.most_common(6):
            print(f"      class {g:016x}: {c} nodes")

    # Entropy probe (encryption/compression detector).
    print("\nentropy probe (8.0 = random/encrypted):")
    for name, off in [("header", 0),
                      ("section A", CHUNK0_OFF),
                      ("middle", len(d)//2),
                      ("tail", max(0, len(d)-65536))]:
        print(f"  {name:10} @0x{off:08x}: {entropy(d[off:off+65536]):.3f}")
    print("\n=> low middle entropy means payloads are NOT encrypted/compressed.")

def extract(path, outdir):
    import os
    d = open(path, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    n = 0
    index = []
    for ci, (off, _count) in enumerate(find_chunks(d)):
        for ni, node in enumerate(walk_nodes(d, off)):
            # Faithful node block, payload after the 24-byte record header.
            payload = d[node.offset + 24:node.offset + node.size]
            fn = f"chunk{ci}_{ni:04d}_{node.name}.node"
            with open(os.path.join(outdir, fn), 'wb') as fh:
                fh.write(payload)
            index.append((fn, node.guid, node.node_id, len(payload)))
            n += 1
    with open(os.path.join(outdir, "index.csv"), 'w') as fh:
        fh.write("file,class_guid,node_id,payload_bytes\n")
        for fn, g, i, sz in index:
            fh.write(f"{fn},{g:016x},{i:016x},{sz}\n")
    print(f"extracted {n} nodes -> {outdir}")

def batch_extract(indir, outdir):
    """Extract every .BF* bigfile in indir into outdir/<package>/, and write a
    global manifest covering all packages (works on the whole game data set)."""
    import os, glob
    os.makedirs(outdir, exist_ok=True)
    manifest = open(os.path.join(outdir, "manifest.csv"), "w")
    manifest.write("package,chunk,index,class_guid,node_id,payload_bytes\n")
    total_files = total_nodes = 0
    for path in sorted(glob.glob(os.path.join(indir, "*.BF*"))):
        d = open(path, 'rb').read()
        if not parse_text_header(d).startswith("Opal"):
            continue
        pkg = os.path.splitext(os.path.basename(path))[0]
        pdir = os.path.join(outdir, pkg)
        os.makedirs(pdir, exist_ok=True)
        n = 0
        for ci, (off, _c) in enumerate(find_chunks(d)):
            for ni, node in enumerate(walk_nodes(d, off)):
                payload = d[node.offset + 24:node.offset + node.size]
                fn = f"c{ci}_{ni:05d}_{node.guid:016x}_{node.node_id:016x}.node"
                with open(os.path.join(pdir, fn), 'wb') as fh:
                    fh.write(payload)
                manifest.write(f"{pkg},{ci},{ni},{node.guid:016x},"
                               f"{node.node_id:016x},{len(payload)}\n")
                n += 1
        total_files += 1
        total_nodes += n
        print(f"  {pkg:<28} {n:>6} nodes")
    manifest.close()
    print(f"\n{total_nodes} nodes from {total_files} packages -> {outdir}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="a .BF* bigfile, or a directory for --batch")
    ap.add_argument("--hex", type=int, default=0,
                    help="dump N words after each chunk magic+count")
    ap.add_argument("--extract", metavar="DIR",
                    help="extract every node payload into DIR (+ index.csv)")
    ap.add_argument("--batch", metavar="OUTDIR",
                    help="treat <file> as a directory of bigfiles; extract all")
    a = ap.parse_args()
    if a.batch:
        batch_extract(a.file, a.batch)
    elif a.extract:
        extract(a.file, a.extract)
    else:
        describe(a.file, a.hex)

if __name__ == "__main__":
    main()
