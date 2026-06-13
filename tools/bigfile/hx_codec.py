#!/usr/bin/env python3
"""
hx_codec.py — decoder for the Opal/Zouna "Hx" texture codec (the compressed
pixel container inside Texture_Z nodes; see docs/OPAL_BIGFILE_FORMAT.md).

Status: the **entropy core is reverse-engineered and validated on real data**.
The codec is a custom per-component DXT/BC transcoder: each BC component is a
sub-stream, Huffman-coded with a per-stream DEFLATE-dynamic table (MSB-first),
then delta-decoded (running sum mod 256). This module implements and validates:

  * the MSB-first bit reader            (engine `sub_95a240`)
  * canonical Huffman decode            (engine `sub_959480`)
  * the DEFLATE-dynamic table builder   (engine `sub_95a020`)
  * the colour-endpoint channel decode  (engine `sub_959590`)

`build_table` + `huff_decode` reproduce the engine exactly: on a real 32×32
texture the colour-endpoint sub-stream decodes its declared entry count while
consuming its sub-block to the byte. The remaining channels (colour indices,
alpha endpoints/indices) share this core but pack their output differently
(3-bit / 2-bit indices via remap table @0xf3ce80) and still need their exact
output formatting + final BC-block interleaving — that is the only remaining
work to emit DDS.
"""
import struct

# Code-length code order (engine table @0xf3ce98) — DEFLATE's permutation,
# extended with RLE symbols 17..20.
PERM = [17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16]

class BitReader:
    """MSB-first bit reader over a byte buffer (engine sub_95a240)."""
    def __init__(self, data):
        self.d = data; self.p = 0; self.buf = 0; self.n = 0
    def bits(self, k):
        if k == 0:
            return 0
        while self.n < k:
            b = self.d[self.p] if self.p < len(self.d) else 0
            self.p += 1
            self.buf = (self.buf | (b << (24 - self.n))) & 0xffffffff
            self.n += 8
        out = self.buf >> (32 - k)
        self.buf = (self.buf << k) & 0xffffffff
        self.n -= k
        return out

def canonical(lengths):
    """Build a canonical Huffman decode table from per-symbol code lengths.
    Returns (table, maxlen) where table maps (length, code) -> symbol, or None."""
    maxlen = max(lengths) if lengths else 0
    if maxlen == 0:
        return None
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code = 0
    next_code = [0] * (maxlen + 1)
    for b in range(1, maxlen + 1):
        code = (code + bl_count[b - 1]) << 1
        next_code[b] = code
        if code > (1 << b):
            return None  # over-subscribed
    table = {}
    for sym, l in enumerate(lengths):
        if l:
            table[(l, next_code[l])] = sym
            next_code[l] += 1
    return table, maxlen

def huff_decode(br, table_maxlen):
    """Decode one symbol with a canonical table (engine sub_959480)."""
    table, maxlen = table_maxlen
    code = 0
    for l in range(1, maxlen + 1):
        code = (code << 1) | br.bits(1)
        if (l, code) in table:
            return table[(l, code)]
    raise ValueError("invalid Huffman code")

def build_table(br):
    """Read a DEFLATE-dynamic Huffman table from the bitstream (engine
    sub_95a020). Returns (table, N) or raises."""
    N = br.bits(14)                       # symbol-count (HLIT), max 0x2000
    if N == 0 or N > 0x2000:
        raise ValueError(f"bad alphabet size N={N}")
    M = br.bits(5)                        # code-length-code count (HCLEN)
    if not (1 <= M <= 21):
        raise ValueError(f"bad HCLEN M={M}")
    meta_len = [0] * 21
    for i in range(M):
        meta_len[PERM[i]] = br.bits(3)
    meta = canonical(meta_len)
    if meta is None:
        raise ValueError("meta table over-subscribed")
    lens = []
    prev = 0
    while len(lens) < N:
        s = huff_decode(br, meta)
        if s <= 16:
            lens.append(s); prev = s
        elif s == 17:
            lens += [0] * (br.bits(3) + 3)
        elif s == 18:
            lens += [0] * (br.bits(7) + 11)
        elif s == 19:
            lens += [prev] * (br.bits(2) + 3)
        elif s == 20:
            lens += [prev] * (br.bits(6) + 7)
        else:
            raise ValueError(f"bad code-length symbol {s}")
    main = canonical(lens[:N])
    if main is None:
        raise ValueError("main table over-subscribed")
    return main, N

def decode_delta_pairs(br, table, count):
    """Colour-endpoint channel (engine sub_959590): `count` entries, each two
    bytes, each byte a running sum (mod 256) of Huffman symbols."""
    a = b = 0
    out = []
    for _ in range(count):
        a = (a + huff_decode(br, table)) & 0xff
        b = (b + huff_decode(br, table)) & 0xff
        out.append((a, b))
    return out

# Hx header field offsets (big-endian) per component decoder.
CHANNELS = {
    "alpha_idx": (0x21, 0x24, 0x27),   # sub_959a20
    "alpha_end": (0x29, 0x2c, 0x2f),   # sub_959ca0
    "color_end": (0x31, 0x34, 0x37),   # sub_959590
    "color_idx": (0x39, 0x3c, 0x3f),   # sub_9596d0
}

def be(d, o, n):
    v = 0
    for i in range(n):
        v = (v << 8) | d[o + i]
    return v

def channel_substream(hx, name):
    """Return (offset, size, count) for a component sub-stream."""
    oo, so, co = CHANNELS[name]
    return be(hx, oo, 3), be(hx, so, 3), be(hx, co, 2)

if __name__ == "__main__":
    import sys
    hx = open(sys.argv[1], "rb").read()
    print(f"Hx blob {len(hx)} bytes  {be(hx,0x0c,2)}x{be(hx,0x0e,2)}  fmt={hx[0x12]}")
    for name in CHANNELS:
        off, size, count = channel_substream(hx, name)
        br = BitReader(hx[off:off + size])
        try:
            tbl, N = build_table(br)
            extra = ""
            if name == "color_end":
                pairs = decode_delta_pairs(br, tbl, count)
                extra = f"  decoded {len(pairs)} pairs, {br.p}/{size} bytes consumed"
            print(f"  {name:10} off={off:5} size={size:4} count={count:3} "
                  f"N={N} maxlen={tbl[1]}{extra}")
        except ValueError as e:
            print(f"  {name:10} ERROR: {e}")
