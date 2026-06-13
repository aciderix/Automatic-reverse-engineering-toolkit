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

Each node record begins with a class GUID (the package's fixed type id),
a node id, the ASCII tag "_OBJ", and per-node metadata (sizes/offsets).
The exact per-field semantics of the metadata are only partially recovered;
this tool surfaces them as raw words so they can be finished against the
node-reader code. The payload data is **not encrypted or compressed**
(measured entropy ≈ 4.4), so node bytes can be sliced out directly.

Usage:
    opal_bigfile.py <file.BFPC>            # describe the container
    opal_bigfile.py <file.BFPC> --hex N    # also dump N words per chunk head
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
        print(f"  @0x{off:08x}  count={count}")
        if hexwords:
            for k in range(off + 8, off + 8 + hexwords * 4, 4):
                v = u32(d, k)
                asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in d[k:k+4])
                print(f"      +0x{k-off:03x}  0x{v:08x}  {asc}")

    # Entropy probe (encryption/compression detector).
    print("\nentropy probe (8.0 = random/encrypted):")
    for name, off in [("header", 0),
                      ("section A", CHUNK0_OFF),
                      ("middle", len(d)//2),
                      ("tail", max(0, len(d)-65536))]:
        print(f"  {name:10} @0x{off:08x}: {entropy(d[off:off+65536]):.3f}")
    print("\n=> low middle entropy means payloads are NOT encrypted/compressed.")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--hex", type=int, default=0,
                    help="dump N words after each chunk magic+count")
    a = ap.parse_args()
    describe(a.file, a.hex)

if __name__ == "__main__":
    main()
